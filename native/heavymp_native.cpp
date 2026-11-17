#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <intrin.h>
#include "heavymp_native.h"
#include "heavymp_collision_policy.h"
#include "heavymp_performance.h"
#include "heavymp_work_budget.h"
#include "heavymp_car_engine_state.h"
#include "heavymp_traffic_capture.h"
#include "heavymp_traffic_lab_wire.h"
#include "heavymp_traffic_motion.h"
#include "heavymp_escort_panel_state.h"
#include "vendor/minhook/include/MinHook.h"
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {
using Ptr = std::uintptr_t;
Ptr engine = 0;
scs_log_t logger = nullptr;
DWORD ownerThread = 0;
std::atomic<bool> enabled{false}, faulted{false};
std::atomic<ULONGLONG> collisionUntil{0};
bool collisionApiReady = false;
bool tractorCollisionApiReady = false;
bool appearanceReady = false;
SRWLOCK nativeLock = SRWLOCK_INIT;
HeavyMpPerfCounter perfWait, perfFrame, perfWheels, perfModels, perfDraw, perfContacts;
HeavyMpFrameCadence frameCadence;
void lockNative() {
    const auto start = heavyMpPerfClock();
    AcquireSRWLockExclusive(&nativeLock);
    perfWait.add(heavyMpPerfClock() - start);
}

using ActorUpdate = void (*)(Ptr);
ActorUpdate originalActorUpdate = nullptr;
void* actorUpdateAddress = nullptr;
using ActorDraw = void (*)(Ptr, Ptr);
ActorDraw originalActorDraw = nullptr;
void* actorDrawAddress = nullptr;
using WheelUpdate = void (*)(Ptr);
WheelUpdate originalWheelUpdate = nullptr;
void* wheelUpdateAddress = nullptr;
std::atomic<unsigned> visible{0};
#include "heavymp_game_build.h"
constexpr auto& fingerprint = HeavyMpGameBuild::sha256;
struct Placement {
    float x, y, z;
    short sx, sz;
    float w, qx, qy, qz;
};
static_assert(sizeof(Placement) == 32);
struct CarEngineEffect {
    Ptr event = 0;
    ULONGLONG created = 0;
    bool started = false;
};
struct Truck {
    HeavyMpVisualBudget visual;
    HeavyMpCollisionAudit collisionAudit;
    HeavyMpPlacementContinuity continuity;
    ULONGLONG collisionGuardAt = 0, loadPollAt = 0;
    HeavyMpCollisionSwitch collisionSwitch;
    bool collisionArmed = false;
    HeavyMpPanelRig panel;
    std::array<unsigned char, 16> id{};
    Ptr definition = 0, physics = 0, world = 0;
    std::array<Ptr, 3> engineEvents{};
    HeavyMpCarEngineState carEngineState{};
    CarEngineEffect carEngineEffect{};
    std::array<std::array<Ptr, 6>, 3> engineParameters{};
    std::array<std::string, 3> loadedAudioReferences{};
    std::array<ULONGLONG, 3> audioCreatedAt{}, audioRetryAt{};
    bool engineSoundFault = false, audioLogged = false;
    std::array<Ptr, 2> hornEvents{};
    bool trailer = false, attached = false, animationDisabled = false, appearanceFallback = false;
    ULONGLONG created = 0, diagnosticAt = 0, submissions = 0, drawCalls = 0, drawAccepted = 0,
              animationAt = 0, wheelUpdates = 0, shadowCalls = 0, sunShadowCalls = 0, engineAudioAt = 0;
    HeavyMpNativePeer animation{};
    std::array<float, 16> suspensionBase{};
    unsigned animationWheels = 0;
    std::shared_ptr<const HeavyMpAppearance> appearance;
    Ptr wiperModel = 0;
    std::array<int, 2> wiperIndices{-1, -1};
    bool wiperBound = false, wiperFault = false, wiperLogged = false;
    ULONGLONG wiperAt = 0;
};
std::vector<Truck> trucks, trailers;
struct PendingDefinition {
    std::array<unsigned char, 16> id{};
    Ptr unit = 0, world = 0;
    std::shared_ptr<const HeavyMpAppearance> appearance;
    HeavyMpDefinitionSlice slice;
};
std::vector<PendingDefinition> pendingDefinitions;

ULONGLONG nextTractorCreation = 0, nextTrailerCreation = 0;
HeavyMpLoadBudget loadBudget;
HeavyMpTagCamera visualCamera;
bool visualCameraValid = false;
bool guardedCameraSnapshot(HeavyMpTagCamera* output);
double workClock() {
    return heavyMpPerfMs(heavyMpPerfClock());
}
void updateVisualBudget(Truck& truck, const HeavyMpNativePeer& peer) {
    truck.visual.distance(
        visualCameraValid ? std::hypot(peer.x - visualCamera.eye[0], peer.z - visualCamera.eye[2]) : -1.0);
}
bool takeLoadPoll(Truck& candidate) {
    unsigned older = 0;
    for (const auto* group : {&trucks, &trailers})
        for (const auto& other : *group) {
            if (&other == &candidate || other.attached || !other.physics)
                continue;
            if (other.loadPollAt < candidate.loadPollAt ||
                (other.loadPollAt == candidate.loadPollAt && other.id < candidate.id))
                if (++older >= 4)
                    return false;
        }
    if (!loadBudget.take(HeavyMpLoadKind::Poll, workClock()))
        return false;
    candidate.loadPollAt = GetTickCount64();
    return true;
}
bool trailerReady = false, trailerFault = false, cargoReady = false;
void log(const char* text) {
    if (logger)
        logger(SCS_LOG_TYPE_message, text);
}
bool readable(Ptr p, std::size_t n) {
    if (!p || !n || p + n < p)
        return false;
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) == sizeof(m) && m.State == MEM_COMMIT &&
           !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           p + n <= reinterpret_cast<Ptr>(m.BaseAddress) + m.RegionSize;
}
template <class T> T& field(Ptr p, std::size_t at) {
    return *reinterpret_cast<T*>(p + at);
}
template <class R, class... A> R method(Ptr p, std::size_t slot, A... args) {
    return reinterpret_cast<R (*)(Ptr, A...)>(field<Ptr>(field<Ptr>(p, 0), slot))(p, args...);
}
template <class R, class... A> R call(std::size_t rva, A... args) {
    return reinterpret_cast<R (*)(A...)>(engine + rva)(args...);
}
bool executableMatches() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, returned = 0;
    bool okay = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                                  sizeof(objectSize), &returned, 0) >= 0;
    std::vector<unsigned char> object(objectSize), buffer(65536);
    std::array<unsigned char, 32> digest{};
    if (okay)
        okay = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0;
    DWORD read = 0;
    while (okay) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            okay = false;
            break;
        }
        if (!read)
            break;
        okay = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (okay)
        okay = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    char hex[65]{};
    for (unsigned i = 0; i < 32; ++i)
        sprintf_s(hex + 2 * i, 3, "%02x", digest[i]);
    return okay && strcmp(hex, fingerprint) == 0;
}
#include "heavymp_environment.inl"
Ptr factory(unsigned descriptor) {
    const unsigned index = field<unsigned>(engine, 0x2794ae8);
    if (index > 3)
        return 0;
    const Ptr manager = field<Ptr>(engine, 0x36e4cf0 + index * 8);
    if (!readable(manager, 8))
        return 0;
    const Ptr unit = call<Ptr>(field<Ptr>(engine, descriptor + 8) - engine, manager, Ptr{0});
    if (!readable(unit, 16))
        return 0;
    InterlockedIncrement(reinterpret_cast<LONG*>(unit + 8));
    InterlockedOr(reinterpret_cast<LONG*>(unit + 8), 0x20000000);
    return unit;
}
void release(Ptr& unit) {
    if (!unit)
        return;
    method<void>(unit, 8);
    const LONG old = InterlockedDecrement(reinterpret_cast<LONG*>(unit + 8)) + 1;
    if ((old & 0x1ffffff) == 1)
        call<void>(0xff360, unit);
    unit = 0;
}
bool string(Ptr destination, const char* text) {
    const std::size_t length = strlen(text);
    if (method<std::size_t>(destination, 0x10, length + 1) < length + 1)
        return false;
    const Ptr buffer = field<Ptr>(destination, 8);
    if (!readable(buffer, length + 1))
        return false;
    memcpy(reinterpret_cast<void*>(buffer), text, length + 1);
    field<unsigned>(destination, 16) = static_cast<unsigned>(length);
    return true;
}
struct Accessory {
    unsigned kind;
    const char* path;
};
// Paths refer to stock ETS2 assets. No meshes or textures are distributed by HeavyMP.
constexpr Accessory stock[] = {{1, "/def/vehicle/truck/daf.xf_euro6/data.sii"},
                               {1, "/def/vehicle/truck/daf.xf_euro6/chassis/4x2.sii"},
                               {1, "/def/vehicle/truck/daf.xf_euro6/cabin/space.sii"},
                               {1, "/def/vehicle/truck/daf.xf_euro6/interior/stan_l_fl.sii"},
                               {1, "/def/vehicle/truck/daf.xf_euro6/engine/mx11_270.sii"},
                               {1, "/def/vehicle/truck/daf.xf_euro6/transmission/zf_12tx2420td.sii"},
                               {1, "/def/vehicle/truck/daf.xf_euro6/head_light/standard.sii"},
                               {4, "/def/vehicle/truck/daf.xf_euro6/paint_job/color2.sii"},
                               {3, "/def/vehicle/f_tire/1.sii"},
                               {3, "/def/vehicle/f_disc/front_disc_02_matt_gray.sii"},
                               {3, "/def/vehicle/f_hub/front_hub_01.sii"},
                               {3, "/def/vehicle/f_nuts/front_nuts_01_steel.sii"},
                               {3, "/def/vehicle/r_tire/1.sii"},
                               {3, "/def/vehicle/r_disc/rear_disc_01_matt_gray.sii"},
                               {3, "/def/vehicle/r_hub/rear_hub_01.sii"},
                               {3, "/def/vehicle/r_nuts/rear_nuts_01_steel.sii"}};
Ptr definition() {
    const Ptr unit = factory(0x1e50500);
    if (!unit)
        return 0;
    for (const auto& item : stock) {
        const unsigned descriptor = item.kind == 1 ? 0x1e53260 : item.kind == 3 ? 0x1e54160 : 0x1e53540;
        Ptr accessory = factory(descriptor);
        if (!accessory || !string(accessory + 0x10, item.path)) {
            if (accessory)
                release(accessory);
            Ptr failed = unit;
            release(failed);
            return 0;
        }
        if (item.kind == 4) {
            float values[18] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0.35f, 0.35f, 0.35f, 0, 0, 0, 0};
            memcpy(reinterpret_cast<void*>(accessory + 0x40), values, sizeof(values));
        }
        const auto count = field<std::size_t>(unit, 0x20);
        if (count >= 64 || !method<bool>(unit + 0x10, 0x18, count + 1)) {
            release(accessory);
            Ptr failed = unit;
            release(failed);
            return 0;
        }
        field<Ptr>(field<Ptr>(unit, 0x18), count * 8) = accessory;
        field<std::size_t>(unit, 0x20) = count + 1;
    }
    return unit;
}
constexpr unsigned appearanceDescriptors[] = {0,         0x1e53260, 0x1e59cc0, 0x1e54160,
                                              0x1e53540, 0x1e53210, 0x1e59c70, 0x1e55540};
constexpr Ptr appearanceVtables[] = {0,         0x2321490, 0x234e828, 0x2328148,
                                     0x2321dd8, 0x23213c8, 0x234e770, 0x232eaa0};
bool cargoToken(Ptr unit, std::uint64_t& id) {
    if (!readable(unit, 0xe8) || field<Ptr>(unit, 0) != engine + 0x234efd0)
        return false;
    const unsigned handle = field<unsigned>(unit, 0xc), index = handle & 0xffff,
                   poolIndex = (handle >> 20) & 0x7ff;
    if (!handle || (handle & 0x80000000) || poolIndex >= 2040)
        return false;
    const Ptr pool = field<Ptr>(engine, 0x30ac380 + poolIndex * 8);
    if (!readable(pool, 0x90) || field<unsigned char>(pool, 0x88) != 2)
        return false;
    const auto count = field<size_t>(pool, 0x50);
    const Ptr names = field<Ptr>(pool, 0), units = field<Ptr>(pool, 0x48);
    if (count > 65536 || index >= count || !readable(names + index * 16, 16) ||
        !readable(units + index * 8, 8) || field<Ptr>(units, index * 8) != unit ||
        field<std::uint64_t>(names, index * 16) != 0x32a4607)
        return false;
    id = field<std::uint64_t>(names, index * 16 + 8);
    return id != 0;
}
// The engine query holds its own registry reader lock. This callback supplies
// bounded result nodes using the same list layout as its native unit queries.
struct CargoQueryNode {
    Ptr next = 0, previous = 0, unit = 0;
};
struct CargoQuery {
    Ptr* vtable = nullptr;
    size_t count = 0;
    Ptr head = 0, tail = 0;
    size_t used = 0;
    std::array<CargoQueryNode, 64> nodes{};
};
Ptr cargoQueryNode(CargoQuery* query) {
    return query->used < query->nodes.size() ? reinterpret_cast<Ptr>(&query->nodes[query->used++]) : 0;
}
Ptr findCargoData(std::uint64_t id) {
    if (!cargoReady || !id)
        return 0;
    Ptr vtable[2] = {0, reinterpret_cast<Ptr>(&cargoQueryNode)};
    CargoQuery query;
    query.vtable = vtable;
    query.head = query.tail = reinterpret_cast<Ptr>(&query.head);
    std::uint64_t tokens[2] = {0x32a4607, id};
    struct TokenArray {
        Ptr vtable;
        std::uint64_t* data;
        size_t count, capacity;
    } names{engine + 0x222c9b8, tokens, 2, 2};
    call<void>(0x12e330, &query, &names);
    for (size_t i = 0; i < query.used; ++i) {
        const Ptr unit = query.nodes[i].unit;
        std::uint64_t found = 0;
        if (cargoToken(unit, found) && found == id && field<LONG>(unit, 8) < 0) {
            InterlockedIncrement(reinterpret_cast<LONG*>(unit + 8));
            return unit;
        }
    }
    return 0;
}
bool accessoryConfiguration(Ptr accessory, const HeavyMpAccessory& item) {
    const size_t bytes = item.kind == 2   ? 0xc8
                         : item.kind == 3 ? 0x58
                         : item.kind == 4 ? 0x90
                         : item.kind == 5 ? 0xa8
                         : item.kind == 7 ? 0x80
                                          : 0x40;
    if (!readable(accessory, bytes) || field<Ptr>(accessory, 0) != engine + appearanceVtables[item.kind])
        return false;
    if (!string(accessory + 0x10, item.path.c_str()))
        return false;
    if (item.kind == 2 && !string(accessory + 0xa8, item.text.c_str()))
        return false;
    if (item.kind == 3) {
        field<unsigned>(accessory, 0x40) = item.flags;
        memcpy(reinterpret_cast<void*>(accessory + 0x44), item.colors.data(), 12);
    }
    if (item.kind == 4)
        memcpy(reinterpret_cast<void*>(accessory + 0x40), item.colors.data(), 72);
    if (item.kind == 7) {
        if (field<Ptr>(accessory, 0x40) || !heavympCargoParams(item.cargoParams))
            return false;
        const Ptr cargo = findCargoData(item.cargoId);
        if (!cargo)
            return false;
        field<Ptr>(accessory, 0x40) = cargo;
        memcpy(reinterpret_cast<void*>(accessory + 0x48), item.cargoParams.data(), item.cargoParams.size());
    }
    if (item.kind == 5) {
        const auto count = item.tokens.size();
        if (count) {
            if (!method<bool>(accessory + 0x40, 0x18, count) ||
                !readable(field<Ptr>(accessory, 0x48), count * 8))
                return false;
            memcpy(reinterpret_cast<void*>(field<Ptr>(accessory, 0x48)), item.tokens.data(), count * 8);
            field<size_t>(accessory, 0x50) = count;
            if (!method<bool>(accessory + 0x68, 0x18, count) ||
                !readable(field<Ptr>(accessory, 0x70), count * 32))
                return false;
            const Ptr elements = field<Ptr>(accessory, 0x70);
            for (size_t i = 0; i < count; ++i) {
                const Ptr s = elements + i * 32;
                memset(reinterpret_cast<void*>(s), 0, 32);
                field<Ptr>(s, 0) = engine + 0x22217a0;
                field<Ptr>(s, 8) = engine + 0x1e2662e;
                // Count includes only constructed strings, even on allocation failure.
                field<size_t>(accessory, 0x78) = i + 1;
                if (!string(s, item.hookups[i].c_str()))
                    return false;
            }
        }
        memcpy(reinterpret_cast<void*>(accessory + 0x98), item.colors.data(), 12);
    }
    return true;
}
Ptr configuredDefinition(std::shared_ptr<const HeavyMpAppearance> appearance,
                         const std::array<unsigned char, 16>& id, bool* waiting) {
    *waiting = false;
    if (!appearance || !heavympValidAppearance(*appearance))
        return 0;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    auto found = std::find_if(pendingDefinitions.begin(), pendingDefinitions.end(), [&](const auto& item) {
        return item.id == id && item.appearance->trailer == appearance->trailer;
    });
    if (found != pendingDefinitions.end() &&
        (found->world != world || found->appearance->revision != appearance->revision)) {
        if (found->world == world)
            release(found->unit);
        pendingDefinitions.erase(found);
        found = pendingDefinitions.end();
    }
    if (found == pendingDefinitions.end()) {
        if (pendingDefinitions.size() >= 160) {
            *waiting = true;
            return 0;
        }
        PendingDefinition item;
        item.id = id;
        item.world = world;
        item.appearance = appearance;
        item.unit = factory(appearance->trailer ? 0x1e50fd0 : 0x1e50500);
        if (!item.unit)
            return 0;
        if (!string(item.unit + 0x70, appearance->plate.c_str())) {
            release(item.unit);
            return 0;
        }
        pendingDefinitions.push_back(std::move(item));
        found = pendingDefinitions.end() - 1;
    }
    Ptr unit = found->unit;
    const auto result = found->slice.advance(
        static_cast<unsigned>(appearance->accessories.size()), workClock, [&](unsigned index) {
            const auto& item = appearance->accessories[index];
            if (item.kind == 7 && !cargoReady)
                return true;
            Ptr accessory = factory(appearanceDescriptors[item.kind]);
            if (!accessory || !accessoryConfiguration(accessory, item)) {
                if (accessory)
                    release(accessory);
                if (item.kind == 7) {
                    log("[HeavyMP cargo] Cargo definition unavailable on this PC; keeping the trailer "
                        "without its load.");
                    return true;
                }
                return false;
            }
            const auto count = field<size_t>(unit, 0x20);
            if (count >= 256 || !method<bool>(unit + 0x10, 0x18, count + 1)) {
                release(accessory);
                return false;
            }
            field<Ptr>(field<Ptr>(unit, 0x18), count * 8) = accessory;
            field<size_t>(unit, 0x20) = count + 1;
            return true;
        });
    if (result == HeavyMpBuildResult::Pending) {
        *waiting = true;
        return 0;
    }
    pendingDefinitions.erase(found);
    if (result == HeavyMpBuildResult::Failed) {
        release(unit);
        return 0;
    }
    return unit;
}
void pruneDefinitions(const HeavyMpNativePeer* peers, unsigned count, bool active, Ptr world) {
    for (auto it = pendingDefinitions.begin(); it != pendingDefinitions.end();) {
        bool present = false;
        if (active && peers)
            for (unsigned i = 0; i < count && !present; ++i) {
                if (!it->appearance->trailer)
                    present = peers[i].id == it->id && peers[i].appearance &&
                              peers[i].appearance->revision == it->appearance->revision;
                auto trailer = peers[i].trailer;
                if (it->appearance->trailer)
                    for (unsigned j = 0; !present && trailer && j < 10; ++j) {
                        present = trailer->id == it->id && trailer->appearance &&
                                  trailer->appearance->revision == it->appearance->revision;
                        trailer = trailer->trailer;
                    }
            }
        if (!present || it->world != world) {
            if (it->world == world)
                release(it->unit);
            it = pendingDefinitions.erase(it);
        } else
            ++it;
    }
}
bool readString(Ptr s, std::string& out, size_t max) {
    if (!readable(s, 24) || field<Ptr>(s, 0) != engine + 0x22217a0)
        return false;
    const auto length = field<unsigned>(s, 16);
    const Ptr buffer = field<Ptr>(s, 8);
    if (length > max || (length && !readable(buffer, length)))
        return false;
    out = length ? std::string(reinterpret_cast<const char*>(buffer), length) : std::string{};
    return heavympText(out, max);
}
bool readAppearanceDefinition(Ptr unit, HeavyMpAppearance& out) {
    if (!readable(unit, 0x90) || field<Ptr>(unit, 0) != engine + (out.trailer ? 0x23156d0 : 0x2310420))
        return false;
    const size_t count = field<size_t>(unit, 0x20);
    const Ptr array = field<Ptr>(unit, 0x18);
    if (count < (out.trailer ? 2u : 7u) || count > 256 || !readable(array, count * 8) ||
        !readString(unit + 0x70, out.plate, 2048))
        return false;
    out.accessories.clear();
    for (size_t i = 0; i < count; ++i) {
        const Ptr accessory = field<Ptr>(array, i * 8);
        if (!readable(accessory, 0x40))
            return false;
        HeavyMpAccessory item;
        for (unsigned k = 1; k <= 7; ++k)
            if (field<Ptr>(accessory, 0) == engine + appearanceVtables[k])
                item.kind = k;
        if (!item.kind || !readString(accessory + 0x10, item.path, 256))
            return false;
        // These markers are generated in memory by the accessory editor; the
        // synthetic filename does not exist in the receiver's SCS archives.
        if (out.trailer && item.kind == 5 &&
            item.path.find("/accessory/markers/trlr_acc_tmp.sii") != std::string::npos)
            continue;
        if (item.kind == 2 && !readString(accessory + 0xa8, item.text, 2048))
            return false;
        if (item.kind == 3) {
            if (!readable(accessory + 0x40, 16))
                return false;
            item.flags = field<unsigned>(accessory, 0x40);
            memcpy(item.colors.data(), reinterpret_cast<void*>(accessory + 0x44), 12);
        }
        if (item.kind == 4) {
            if (!readable(accessory + 0x40, 72))
                return false;
            memcpy(item.colors.data(), reinterpret_cast<void*>(accessory + 0x40), 72);
        }
        if (item.kind == 7) {
            if (!out.trailer || !readable(accessory, 0x80) ||
                !cargoToken(field<Ptr>(accessory, 0x40), item.cargoId))
                return false;
            memcpy(item.cargoParams.data(), reinterpret_cast<void*>(accessory + 0x48),
                   item.cargoParams.size());
        }
        if (item.kind == 5) {
            if (!readable(accessory, 0xa8))
                return false;
            const auto tokens = field<size_t>(accessory, 0x50), hookups = field<size_t>(accessory, 0x78);
            const Ptr a = field<Ptr>(accessory, 0x48), b = field<Ptr>(accessory, 0x70);
            if (tokens > 256 || tokens != hookups ||
                (tokens && (!readable(a, tokens * 8) || !readable(b, hookups * 32))))
                return false;
            for (size_t j = 0; j < tokens; ++j) {
                item.tokens.push_back(field<std::uint64_t>(a, j * 8));
                std::string s;
                if (!readString(b + j * 32, s, 128))
                    return false;
                item.hookups.push_back(std::move(s));
            }
            memcpy(item.colors.data(), reinterpret_cast<void*>(accessory + 0x98), 12);
        }
        out.accessories.push_back(std::move(item));
    }
    return heavympValidAppearance(out);
}
bool readLocalAppearance(HeavyMpAppearance* out) {
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!out || !readable(world, 0x31b8))
        return false;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x20))
        return false;
    const Ptr physics = field<Ptr>(actor, 0x18);
    if (!readable(physics, 0x200) || field<Ptr>(physics, 0) != engine + 0x2332578)
        return false;
    return readAppearanceDefinition(field<Ptr>(physics, 0x1f8), *out);
}
Ptr localTrailerPhysics(unsigned segment) {
    if (segment >= 10)
        return 0;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8))
        return 0;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0xd0))
        return 0;
    Ptr physics = field<Ptr>(actor, 0xc8);
    std::array<Ptr, 10> seen{};
    for (unsigned i = 0; i <= segment; ++i) {
        if (!readable(physics, 0x1060) || field<Ptr>(physics, 0) != engine + 0x22d4b58 ||
            std::find(seen.begin(), seen.begin() + i, physics) != seen.begin() + i)
            return 0;
        seen[i] = physics;
        if (i == segment)
            return physics;
        physics = field<Ptr>(physics, 0x1058);
    }
    return 0;
}
bool readLocalTrailerAppearance(HeavyMpAppearance* out, unsigned segment = 0) {
    if (!out)
        return false;
    const Ptr physics = localTrailerPhysics(segment);
    if (!physics)
        return false;
    out->trailer = true;
    return readAppearanceDefinition(field<Ptr>(physics, 0x1f8), *out);
}
bool guardedLocalTrailerAppearance(HeavyMpAppearance* out, unsigned segment = 0) {
    __try {
        return readLocalTrailerAppearance(out, segment);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool guardedLocalAppearance(HeavyMpAppearance* out) {
    __try {
        return readLocalAppearance(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
Placement placement(const HeavyMpNativePeer& p) {
    const double h = p.heading * 3.141592653589793, pitch = p.pitch * 3.141592653589793,
                 r = p.roll * 3.141592653589793;
    const double ch = cos(h), sh = sin(h), cp = cos(pitch), sp = sin(pitch), cr = cos(r), sr = sin(r);
    const double sx = floor(p.x / 512), sz = floor(p.z / 512);
    return {static_cast<float>(p.x - sx * 512),
            static_cast<float>(p.y),
            static_cast<float>(p.z - sz * 512),
            static_cast<short>(sx),
            static_cast<short>(sz),
            static_cast<float>(ch * cp * cr + sh * sp * sr),
            static_cast<float>(ch * sp * cr + sh * cp * sr),
            static_cast<float>(sh * cp * cr - ch * sp * sr),
            static_cast<float>(ch * cp * sr - sh * sp * cr)};
}
#include "heavymp_siren_latch.h"
#include "heavymp_vehicle_audio.h"
#include "heavymp_horns.inl"
#include "heavymp_engine_audio.inl"
#include "heavymp_car_fmod.inl"
#include "heavymp_car_engine.inl"
#include "heavymp_siren.inl"
#include "heavymp_wipers.inl"
#include "heavymp_escort_panel.inl"
double wheelRadians(float rotations, float rotationsPerSecond, double elapsed) {
    // SDK: positive turns/rps = forward. Native driveline phases: negative
    // radians = forward (verified against the actual local tractor/trailers).
    const double phase = rotations + rotationsPerSecond * std::clamp(elapsed, 0., .65);
    return -(phase - floor(phase)) * 6.283185307179586;
}
bool animateTractor(Truck& t) {
    constexpr double tau = 6.283185307179586;
    const Ptr physics = t.physics, drive = field<Ptr>(physics, 0x150), steer = field<Ptr>(physics, 0x6c0);
    const auto& a = t.animation;
    if (a.controls & 8)
        field<unsigned>(physics, 0x2a0) = a.lights & ~heavyMpPanelLightBit;
    if (!a.wheelCount || !readable(drive, 0x320))
        return false;
    // ETS2 1.62.3.3's wheel phase getter (0x8e69a0) uses a vector
    // at 0x308 whose storage pointer and count are at 0x310/0x318.
    const auto count = field<std::size_t>(drive, 0x318);
    const Ptr phases = field<Ptr>(drive, 0x310);
    if (!count || count > 16 || !readable(phases, count * sizeof(float)))
        return false;
    // Both SDK and native data enumerate left/right wheels in axle order.
    // The stock DAF has fewer axles than some source trucks: retain its rear
    // axle while taking the corresponding pair from the last source axle.
    auto source = [&](unsigned i) {
        if (a.wheelCount == count)
            return i;
        if (i < 2)
            return std::min(i, a.wheelCount - 1);
        return a.wheelCount >= 2 ? a.wheelCount - 2 + (i & 1u) : 0u;
    };
    const ULONGLONG now = GetTickCount64();
    const double elapsed =
        t.animationAt && now >= t.animationAt ? std::min((now - t.animationAt) / 1000.0, .65) : 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto& w = a.wheels[source(i)];
        field<float>(phases, i * 4) = static_cast<float>(wheelRadians(w.rotation, w.velocity, elapsed));
    }
    // Steering stores angles per steering group, plus a wheel->group map.
    // Do not assume that its array is the same size as the wheel array.
    if (readable(steer, 0xb8)) {
        const auto groups = field<std::size_t>(steer, 0x40), mapped = field<std::size_t>(steer, 0xb0);
        const Ptr angles = field<Ptr>(steer, 0x38), map = field<Ptr>(steer, 0xa8);
        if (groups <= 16 && mapped >= count && mapped <= 16 && readable(angles, groups * 4) &&
            readable(map, mapped * 8)) {
            float sums[16]{};
            unsigned samples[16]{};
            for (unsigned i = 0; i < count; ++i) {
                const auto group = field<std::size_t>(map, i * 8);
                if (group >= groups)
                    continue;
                sums[group] += a.wheels[source(i)].steering;
                ++samples[group];
            }
            for (unsigned i = 0; i < groups; ++i)
                if (samples[i])
                    field<float>(angles, i * 4) = static_cast<float>(sums[i] / samples[i] * tau);
        }
    }
    const Ptr lift = field<Ptr>(physics, 0x6c8);
    if (readable(lift, 0x28)) {
        const auto axles = field<std::size_t>(lift, 0x20);
        const Ptr values = field<Ptr>(lift, 0x18);
        if (axles == count / 2 && axles <= 8 && readable(values, axles * 4))
            for (unsigned i = 0; i < axles; ++i)
                field<float>(values, i * 4) =
                    (a.wheels[source(i * 2)].lift + a.wheels[source(i * 2 + 1)].lift) * .5f;
    }
    const Ptr wheelArray = field<Ptr>(physics, 0x530);
    if (field<std::size_t>(physics, 0x538) == count && readable(wheelArray, count * 8)) {
        if (t.animationWheels != count) {
            for (unsigned i = 0; i < count; ++i) {
                const Ptr wheel = field<Ptr>(wheelArray, i * 8);
                if (readable(wheel, 0x30))
                    t.suspensionBase[i] = field<float>(wheel, 0x2c);
            }
            t.animationWheels = static_cast<unsigned>(count);
        }
        for (unsigned i = 0; i < count; ++i) {
            const Ptr wheel = field<Ptr>(wheelArray, i * 8);
            if (readable(wheel, 0x30))
                field<float>(wheel, 0x2c) =
                    std::max(0.f, t.suspensionBase[i] + a.wheels[source(i)].suspension);
        }
    }
    return true;
}
bool readLocalLights(unsigned* lights) {
    if (!lights || !enabled || faulted)
        return false;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8))
        return false;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x20))
        return false;
    const Ptr physics = field<Ptr>(actor, 0x18);
    if (!readable(physics, 0x2a4) || field<Ptr>(physics, 0) != engine + 0x2332578)
        return false;
    *lights = field<unsigned>(physics, 0x2a0);
    return true;
}
bool guardedLocalLights(unsigned* lights) {
    __try {
        return readLocalLights(lights);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void disableArray(Ptr array, std::size_t count) {
    if (count > 512 || !count || !readable(array, count * 8))
        return;
    for (std::size_t i = 0; i < count; ++i) {
        const Ptr body = field<Ptr>(array, i * 8);
        if (!readable(body, 0xb1))
            continue;
        if (field<unsigned char>(body, 0xb0))
            method<void>(body, 16);
    }
}
void prepareRemoteBody(Ptr collection) {
    if (!readable(collection, 0x168))
        return;
    // Body collection virtual 0x190 enables the engine's remotely placed mode.
    // This synchronizes its placement history for a remotely driven body and
    // must precede disabling collisions. Actual visibility still needs a game test.
    const Ptr table = field<Ptr>(collection, 0);
    if (!readable(table, 0x1a0) || field<Ptr>(table, 0x190) != engine + 0x1672500)
        return;
    method<void>(collection, 0x190, static_cast<unsigned char>(1));
}
void disableBodies(Ptr physics, bool trailer = false) {
    constexpr unsigned collections[] = {0x30u, 0x1278u};
    for (unsigned c = 0; c < (trailer ? 1u : 2u); ++c) {
        const unsigned offset = collections[c];
        const Ptr collection = field<Ptr>(physics, offset);
        if (!readable(collection, 32))
            continue;
        disableArray(field<Ptr>(collection, 16), field<std::size_t>(collection, 24));
    }
    // Cab, wheels, suspension and attached accessories also own physical bodies.
    // Only bodies belonging to this HeavyMP-created tractor are affected.
    constexpr unsigned tractorArrays[] = {0x48u, 0xb8u, 0xed0u, 0xf00u, 0xf88u, 0x13d0u, 0x13f8u};
    constexpr unsigned trailerArrays[] = {0x48u, 0xb8u, 0xed0u, 0xf00u, 0xf88u, 0xfd0u, 0x10a8u};
    for (const unsigned offset : trailer ? trailerArrays : tractorArrays)
        disableArray(field<Ptr>(physics, offset), field<std::size_t>(physics, offset + 8));
}
void applyRemoteCollisions(Truck& truck, bool collide, bool trailer = false) {
    const Ptr physics = truck.physics;
    if (!truck.collisionAudit.due(collide, GetTickCount64()))
        return;
    const auto perfStart = heavyMpPerfClock();
    struct ContactTimer {
        std::uint64_t start;
        ~ContactTimer() {
            perfContacts.add(heavyMpPerfClock() - start);
        }
    } timer{perfStart};
    if (!trailer) {
        // truck_physics virtual 0x198 forwards to 0x863e80 (base shapes) and
        // separately enables/disables the cabin array at +0x13f8/+0x1400.
        // Toggling only the primary body collections misses this cabin array.
        if (!tractorCollisionApiReady) {
            disableBodies(physics);
            return;
        }
        truck.collisionSwitch.apply(collide,
                                    [&](unsigned char ghost) { method<void>(physics, 0x198, ghost); });
    }
    if (!collide || !collisionApiReady) {
        disableBodies(physics, trailer);
        return;
    }
    // Only the remotely placed chassis/cab collections are collision proxies.
    // Wheels/accessories stay disabled: their dynamics are driven by telemetry.
    constexpr unsigned collections[] = {0x30u, 0x1278u};
    for (unsigned c = 0; c < (trailer ? 1u : 2u); ++c) {
        const unsigned offset = collections[c];
        const Ptr collection = field<Ptr>(physics, offset);
        if (!readable(collection, 32))
            continue;
        const auto count = field<std::size_t>(collection, 24);
        const Ptr array = field<Ptr>(collection, 16);
        if (!count || count > 512 || !readable(array, count * 8))
            continue;
        for (std::size_t i = 0; i < count; ++i) {
            const Ptr body = field<Ptr>(array, i * 8);
            // Verified ETS2 1.62.3.3s body wrapper: +8 enables contact flags,
            // +16 disables them; never call an unverified object's virtual API.
            if (!readable(body, 0xb1) || field<Ptr>(body, 0) != engine + 0x249ec48)
                continue;
            if (!trailer) {
                const unsigned category = (field<unsigned>(body, 0xa4) >> 5) & 31u;
                const unsigned remote = heavyMpRemoteContactCategory(category);
                // +0x30 updates both PhysX simulation/query filter data and
                // the parent's cached category through the verified API.
                if (remote != category)
                    method<void>(body, 0x30, remote);
            }
            if (!field<unsigned char>(body, 0xb0))
                method<void>(body, 8);
        }
    }
}
bool remoteCollisionAllowed(Truck& t, const HeavyMpNativePeer& peer) {
    if (peer.zoneGhost) {
        t.collisionArmed = false;
        return false;
    }
    if (!t.trailer && !tractorCollisionApiReady) {
        t.collisionArmed = false;
        return false;
    }
    if (!collisionApiReady || !heavyMpCollisionLease(GetTickCount64(), collisionUntil.load())) {
        t.collisionArmed = false;
        return false;
    }
    if (t.collisionArmed)
        return true;
    if (GetTickCount64() - std::max(t.created, t.collisionGuardAt) < 3000)
        return false;
    if (!readable(t.world, 0x31b8))
        return false;
    const Ptr actor = field<Ptr>(t.world, 0x31b0);
    if (!readable(actor, 0x20))
        return false;
    const Ptr local = field<Ptr>(actor, 0x18);
    if (!readable(local, 0x2a4) || field<Ptr>(local, 0) != engine + 0x2332578)
        return false;
    const auto overlaps = [&](Ptr physics) {
        Placement pose{};
        method<void>(physics, 0xf8, &pose);
        const double dx = peer.x - (pose.x + static_cast<double>(pose.sx) * 512),
                     dz = peer.z - (pose.z + static_cast<double>(pose.sz) * 512);
        return std::abs(peer.y - pose.y) < 6 && dx * dx + dz * dz < 64;
    };
    // Initial ghost protection includes the local trailer chain. Once armed,
    // contact must NOT turn it off, otherwise every impact would pass through.
    if (overlaps(local))
        return false;
    for (unsigned i = 0; i < 10; ++i) {
        const Ptr trailer = localTrailerPhysics(i);
        if (!trailer)
            break;
        if (overlaps(trailer))
            return false;
    }
    t.collisionArmed = true;
    return true;
}
void prepareRemotePlacement(Truck& t, const HeavyMpNativePeer& peer) {
    const auto now = GetTickCount64();
    if (t.continuity.update(peer.x, peer.y, peer.z, peer.speed, now)) {
        t.collisionArmed = false;
        t.collisionGuardAt = now;
        t.collisionAudit = {};
        applyRemoteCollisions(t, false, t.trailer);
        log("[HeavyMP collision] Remote relocation: contacts disabled before placement; separation guard "
            "restarted.");
    }
}
void diagnostic(Truck& t, const Placement& requested) {
    const ULONGLONG now = GetTickCount64();
    if (t.diagnosticAt && now - t.diagnosticAt < 5000)
        return;
    t.diagnosticAt = now;
    Placement actual{};
    method<void>(t.physics, 0xf8, &actual);
    const Ptr body = field<Ptr>(t.physics, 0x30), draw = field<Ptr>(t.physics, 0x38);
    char message[512]{};
    sprintf_s(message,
              "[HeavyMP native 0.6.0] remote-mode=%u requested=(%.2f,%.2f,%.2f;%d,%d) "
              "engine=(%.2f,%.2f,%.2f;%d,%d) draw-state=%u updates=%llu draw-calls=%llu accepted=%llu",
              readable(body, 0x165) ? field<unsigned char>(body, 0x164) : 0, requested.x, requested.y,
              requested.z, requested.sx, requested.sz, actual.x, actual.y, actual.z, actual.sx, actual.sz,
              readable(draw, 4) ? field<unsigned>(draw, 0) : 0, t.submissions, t.drawCalls, t.drawAccepted);
    log(message);
    const Ptr cabin = field<Ptr>(t.physics, 0x1278);
    sprintf_s(message,
              "[HeavyMP collisions] tractor policy=%u armed=%u applied=%d native-ghost=%u cabin-shapes=%llu "
              "contact-category=%u/%u simulation=%u/%u",
              heavyMpCollisionLease(now, collisionUntil.load()) ? 1u : 0u, t.collisionArmed ? 1u : 0u,
              t.collisionSwitch.applied, field<unsigned char>(t.physics, 0x766),
              static_cast<unsigned long long>(field<std::size_t>(t.physics, 0x1400)),
              readable(body, 0x94) ? (field<unsigned>(body, 0x90) >> 6) & 63u : ~0u,
              readable(cabin, 0x94) ? (field<unsigned>(cabin, 0x90) >> 6) & 63u : ~0u,
              readable(body, 0x167) ? field<unsigned char>(body, 0x166) : 0,
              readable(cabin, 0x167) ? field<unsigned char>(cabin, 0x166) : 0);
    log(message);
    sprintf_s(message,
              "[HeavyMP animation] wheels=%u rotation=%.3f velocity=%.3f steering=%.3f suspension=%.3f "
              "lights=%x rpm=%.0f gear=%d controls=%x wheel-updates=%llu disabled=%u",
              t.animation.wheelCount, t.animation.wheels[0].rotation, t.animation.wheels[0].velocity,
              t.animation.wheels[0].steering, t.animation.wheels[0].suspension, t.animation.lights,
              t.animation.rpm, t.animation.gear, t.animation.controls, t.wheelUpdates,
              t.animationDisabled ? 1u : 0u);
    log(message);
}
void remove(Truck& t, Ptr world) {
    guardedFreeHorns(t);
    guardedFreeEngineAudio(t);
    // An unloaded scene may have invalidated dependencies. Do not dereference it.
    if (t.world && t.world != world) {
        t.physics = 0;
        t.definition = 0;
        t.attached = false;
        return;
    }
    if (t.attached && t.physics) {
        call<void>(0x47d9b0, world, t.physics);
        t.attached = false;
    }
    if (t.physics)
        release(t.physics);
    if (t.definition)
        release(t.definition);
}
void tick(const HeavyMpNativePeer* peers, unsigned count, bool active) {
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!ownerThread)
        ownerThread = GetCurrentThreadId();
    if (ownerThread != GetCurrentThreadId())
        return;
    pruneDefinitions(peers, count, active, world);
    for (auto& t : trucks) {
        const bool present =
            active && count && peers &&
            std::find_if(peers, peers + count, [&](const auto& p) { return p.id == t.id; }) != peers + count;
        if (!present || t.world != world)
            remove(t, world);
    }
    trucks.erase(std::remove_if(trucks.begin(), trucks.end(), [](const Truck& t) { return !t.physics; }),
                 trucks.end());
    visible = 0;
    if (!active || !readable(world, 0x2df0))
        return;
    for (unsigned i = 0; i < count; ++i) {
        const auto& peer = peers[i];
        if (!std::isfinite(peer.x) || !std::isfinite(peer.y) || !std::isfinite(peer.z) ||
            std::abs(peer.x) > 16000000 || std::abs(peer.z) > 16000000)
            continue;
        auto found =
            std::find_if(trucks.begin(), trucks.end(), [&](const Truck& t) { return t.id == peer.id; });
        if (found != trucks.end() && peer.appearance &&
            (!found->appearance || found->appearance->revision != peer.appearance->revision)) {
            remove(*found, world);
            trucks.erase(found);
            found = trucks.end();
        }
        if (found == trucks.end()) {
            if (trucks.size() >= 32 || GetTickCount64() < nextTractorCreation ||
                !loadBudget.take(HeavyMpLoadKind::Create, workClock()))
                continue;
            nextTractorCreation = GetTickCount64() + 150;
            Truck t{};
            t.id = peer.id;
            t.world = world;
            t.created = GetTickCount64();
            t.appearance = peer.appearance;
            bool waiting = false;
            t.definition = t.appearance && appearanceReady
                               ? configuredDefinition(t.appearance, peer.id, &waiting)
                               : definition();
            if (waiting) {
                nextTractorCreation = 0;
                continue;
            }
            if (!t.definition) {
                t.appearanceFallback = true;
                t.definition = definition();
                log("[HeavyMP appearance] Definition could not be constructed; using fallback tractor.");
            }
            if (!t.definition)
                continue;
            t.physics = factory(0x1e55cf0);
            if (!t.physics) {
                release(t.definition);
                continue;
            }
            method<void>(t.physics, 0x60, t.definition, unsigned{0x401});
            trucks.push_back(t);
            found = trucks.end() - 1;
            if (t.appearance) {
                char message[360]{};
                sprintf_s(message,
                          "[HeavyMP appearance] Remote configuration requested: accessories=%zu first=%s "
                          "fallback=%u",
                          t.appearance->accessories.size(), t.appearance->accessories[0].path.c_str(),
                          t.appearanceFallback ? 1u : 0u);
                log(message);
            } else
                log("[HeavyMP native] Stock ETS2 tractor requested; awaiting engine model loading.");
        }
        Truck& t = *found;
        updateVisualBudget(t, peer);
        t.animation = peer;
        t.animationAt = peer.wheelSampleAt ? peer.wheelSampleAt : GetTickCount64();
        if (!readable(t.physics, 0x1408))
            continue;
        if (!t.attached) {
            if (!takeLoadPoll(t))
                continue;
            if (t.appearance && !t.appearanceFallback && GetTickCount64() - t.created > 10000) {
                release(t.physics);
                release(t.definition);
                t.appearanceFallback = true;
                t.created = GetTickCount64();
                t.collisionSwitch = {};
                t.collisionAudit = {};
                t.continuity = {};
                t.collisionArmed = false;
                t.definition = definition();
                t.physics = t.definition ? factory(0x1e55cf0) : 0;
                if (t.physics)
                    method<void>(t.physics, 0x60, t.definition, unsigned{0x401});
                log("[HeavyMP appearance] Configured assets did not load within 10 seconds. Check installed "
                    "DLC; using fallback tractor.");
                continue;
            }
            // Advance the engine's asynchronous loading state, as its own virtual API does.
            const Ptr request = field<Ptr>(t.physics, 0xc70);
            if (readable(request, 0x28) && field<unsigned char>(request, 0x24) == 0 &&
                field<unsigned>(request, 0x20) == 3)
                InterlockedExchange(reinterpret_cast<LONG*>(request + 0x20), 4);
            method<void>(t.physics, 0x68);
            if (field<unsigned>(t.physics, 0xc68) < 6)
                continue;
            if (!loadBudget.take(HeavyMpLoadKind::Attach, workClock()))
                continue;
            method<void>(t.physics, 0x80, t.definition, unsigned{0x401});
            if (!field<Ptr>(t.physics, 0x30) || !field<Ptr>(t.physics, 0x200))
                continue;
            call<void>(0x85eb50, t.physics);
            prepareRemoteBody(field<Ptr>(t.physics, 0x30));
            prepareRemoteBody(field<Ptr>(t.physics, 0x1278));
            field<unsigned char>(t.physics, 0x25b) = 0;
            disableBodies(t.physics);
            const Placement pose = placement(peer);
            method<void>(t.physics, 0x110, &pose);
            call<void>(0x47d910, world, t.physics);
            t.attached = true;
            log("[HeavyMP native] ETS2 tractor attached to current world; visual verification required.");
        }
        prepareRemotePlacement(t, peer);
        const Placement pose = placement(peer);
        method<void>(t.physics, 0x110, &pose);
        applyRemoteCollisions(t, remoteCollisionAllowed(t, peer));
        guardedHorns(t);
        guardedEngineAudio(t);
        diagnostic(t, pose);
        ++visible;
    }
}
void tickTrailers(const HeavyMpNativePeer* owners, unsigned ownerCount, bool active) {
    std::vector<HeavyMpNativePeer> segments;
    segments.reserve(std::min(ownerCount * 2, 128u));
    for (unsigned i = 0; i < ownerCount; ++i) {
        auto segment = owners[i].trailer;
        for (unsigned j = 0; segment && j < 10; ++j) {
            auto parent = owners[i];
            parent.id = segment->id;
            parent.trailer = segment;
            segments.push_back(parent);
            segment = segment->trailer;
        }
    }
    const auto* peers = segments.data();
    const unsigned count = static_cast<unsigned>(segments.size());
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (ownerThread != GetCurrentThreadId())
        return;
    for (auto& t : trailers) {
        const auto p = peers && count
                           ? std::find_if(peers, peers + count,
                                          [&](const auto& p) {
                                              return p.id == t.id && p.trailer && p.trailer->appearance;
                                          })
                           : peers;
        if (!active || !p || p == peers + count || t.world != world ||
            p->trailer->appearance->revision != t.appearance->revision)
            remove(t, world);
    }
    trailers.erase(
        std::remove_if(trailers.begin(), trailers.end(), [](const Truck& t) { return !t.physics; }),
        trailers.end());
    if (!trailerReady || trailerFault || !active || !readable(world, 0x31b8))
        return;
    for (unsigned i = 0; i < count; ++i) {
        const auto& tractor = peers[i];
        if (!tractor.trailer || !tractor.trailer->appearance)
            continue;
        const auto& peer = *tractor.trailer;
        auto found = std::find_if(trailers.begin(), trailers.end(),
                                  [&](const Truck& t) { return t.id == tractor.id; });
        if (found == trailers.end()) {
            if (trailers.size() >= 128 || GetTickCount64() < nextTrailerCreation ||
                !loadBudget.take(HeavyMpLoadKind::Create, workClock()))
                continue;
            nextTrailerCreation = GetTickCount64() + 100;
            Truck t{};
            t.trailer = true;
            t.id = tractor.id;
            t.world = world;
            t.created = GetTickCount64();
            t.appearance = peer.appearance;
            bool waiting = false;
            t.definition = configuredDefinition(t.appearance, tractor.id, &waiting);
            if (waiting) {
                nextTrailerCreation = 0;
                continue;
            }
            if (!t.definition)
                continue;
            t.physics = factory(0x1e4c2b0);
            if (!t.physics) {
                release(t.definition);
                continue;
            }
            method<void>(t.physics, 0x60, t.definition, unsigned{0x40a});
            trailers.push_back(t);
            found = trailers.end() - 1;
            log("[HeavyMP trailer] Single trailer requested with original ETS2 accessories.");
        }
        Truck& t = *found;
        updateVisualBudget(t, peer);
        t.animation = peer;
        t.animation.lights = tractor.lights;
        t.animation.controls = tractor.controls;
        t.animationAt = peer.wheelSampleAt ? peer.wheelSampleAt : GetTickCount64();
        if (!readable(t.physics, 0x12b0))
            continue;
        if (!t.attached) {
            if (!takeLoadPoll(t))
                continue;
            if (GetTickCount64() - t.created > 15000) {
                remove(t, world);
                log("[HeavyMP trailer] Assets failed to load; check installed DLC. Tractor preserved.");
                continue;
            }
            const Ptr request = field<Ptr>(t.physics, 0xc70);
            if (readable(request, 0x28) && field<unsigned char>(request, 0x24) == 0 &&
                field<unsigned>(request, 0x20) == 3)
                InterlockedExchange(reinterpret_cast<LONG*>(request + 0x20), 4);
            method<void>(t.physics, 0x68);
            if (!method<unsigned char>(t.physics, 0x78))
                continue;
            if (!loadBudget.take(HeavyMpLoadKind::Attach, workClock()))
                continue;
            method<void>(t.physics, 0x80, t.definition, unsigned{0x40a});
            if (!field<Ptr>(t.physics, 0x30) || !field<Ptr>(t.physics, 0x200))
                continue;
            call<void>(0x85eb50, t.physics);
            prepareRemoteBody(field<Ptr>(t.physics, 0x30));
            // No physical coupling is created: remote SDK placement supplies articulation.
            method<void>(t.physics, 0x328, Ptr{0});
            disableBodies(t.physics, true);
            const auto pose = placement(peer);
            method<void>(t.physics, 0x110, &pose);
            call<void>(0x47d910, world, t.physics);
            t.attached = true;
            log("[HeavyMP trailer] Native single trailer attached; visual verification required.");
        }
        prepareRemotePlacement(t, peer);
        const auto pose = placement(peer);
        method<void>(t.physics, 0x110, &pose);
        applyRemoteCollisions(t, remoteCollisionAllowed(t, peer), true);
    }
    trailers.erase(
        std::remove_if(trailers.begin(), trailers.end(), [](const Truck& t) { return !t.physics; }),
        trailers.end());
}
bool guardedTrailers(const HeavyMpNativePeer* peers, unsigned count, bool active) {
    __try {
        tickTrailers(peers, count, active);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Keep structured exception handling outside functions with C++ destructors.
bool guardedTick(const HeavyMpNativePeer* peers, unsigned count, bool active) {
    __try {
        tick(peers, count, active);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool wheelPhase(Ptr localPhysics, Ptr* currentWorld) {
    if (!enabled || faulted)
        return false;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x3268))
        return false;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x20) || field<Ptr>(actor, 0x18) != localPhysics)
        return false;
    // 0x481040 writes to this active frame. Outside the wheel update phase its
    // index is SIZE_MAX, which caused the 0.5.4 assertion at the actor callback.
    const Ptr frames = field<Ptr>(world, 0x3260);
    if (!readable(frames, 0x2d0))
        return false;
    const auto count = field<std::size_t>(frames, 0x10), index = field<std::size_t>(frames, 0x2c8);
    if (index >= count)
        return false;
    *currentWorld = world;
    return true;
}
bool guardedWheelPhase(Ptr physics, Ptr* world) {
    __try {
        return wheelPhase(physics, world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool guardedAnimate(Truck* t) {
    __try {
        const bool wheelsApplied = animateTractor(*t);
        guardedWipers(*t);
        method<void>(t->physics, 0xa0);
        if (wheelsApplied)
            ++t->wheelUpdates;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void updateTractorWheels(Ptr localPhysics) {
    Ptr world = 0;
    if (!guardedWheelPhase(localPhysics, &world))
        return;
    for (auto* group : {&trucks, &trailers})
        for (auto& t : *group) {
            if (!t.attached || t.animationDisabled || !t.physics || t.world != world ||
                (t.trailer && trailerFault))
                continue;
            if (!t.visual.wheels(GetTickCount64()))
                continue;
            if (!guardedAnimate(&t)) {
                t.animationDisabled = true;
                log("[HeavyMP native 0.6.0] Wheel animation fault; animations disabled for this tractor, "
                    "native drawing preserved.");
            }
        }
}
void wheelUpdate(Ptr physics) {
    originalWheelUpdate(physics);
    // Remote virtual 0xa0 re-enters this hook. Reject non-local physics before
    // locking, so the nested remote call never acquires the same SRW lock.
    Ptr world = 0;
    if (!guardedWheelPhase(physics, &world))
        return;
    lockNative();
    const auto start = heavyMpPerfClock();
    updateTractorWheels(physics);
    perfWheels.add(heavyMpPerfClock() - start);
    ReleaseSRWLockExclusive(&nativeLock);
}
bool guardedTrailerSubmit(Truck* t) {
    __try {
        if (t->animation.controls & 8)
            field<unsigned>(t->physics, 0x2a0) = t->animation.lights & ~heavyMpPanelLightBit;
        method<void>(t->physics, 0xa8);
        ++t->submissions;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#include "heavymp_repair.inl"
#include "heavymp_dispatch_live.inl"
void submitTractors(Ptr actor) {
    if (!enabled || faulted || !ownerThread || ownerThread != GetCurrentThreadId())
        return;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8) || field<Ptr>(world, 0x31b0) != actor)
        return;
    processRepair(actor);
    for (auto* group : {&trucks, &trailers})
        for (auto& t : *group) {
            if (!t.attached || !t.physics || t.world != world || (t.trailer && trailerFault))
                continue;
            if (!t.visual.models(GetTickCount64(), t.animation.lights))
                continue;
            if (t.trailer) {
                if (!guardedTrailerSubmit(&t)) {
                    trailerFault = true;
                    log("[HeavyMP trailer] Model update fault; trailer sync disabled, tractor drawing "
                        "preserved.");
                }
                continue;
            }
            // Actor updates run outside the placement-frame write phase. Wheels
            // are updated only from the native local wheel callback above.
            if (t.animation.controls & 8)
                field<unsigned>(t.physics, 0x2a0) = t.animation.lights & ~heavyMpPanelLightBit;
            // Virtual 0xa8 updates model nodes and animations in the actor phase.
            // It does not draw meshes. Virtual 0x300 below performs that separate step.
            method<void>(t.physics, 0xa8);
            ++t.submissions;
        }
}
bool guardedSubmit(Ptr actor) {
    __try {
        submitTractors(actor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void actorUpdate(Ptr actor) {
    originalActorUpdate(actor);
    lockNative();
    const auto start = heavyMpPerfClock();
    if (!guardedSubmit(actor)) {
        faulted = true;
        visible = 0;
        log("[HeavyMP native 0.6.0] Model update fault; bridge stopped. Restart ETS2.");
    }
    perfModels.add(heavyMpPerfClock() - start);
    ReleaseSRWLockExclusive(&nativeLock);
}
bool guardedTrailerDraw(Ptr physics, Ptr context) {
    __try {
        method<void>(physics, 0x300, context, static_cast<unsigned char>(0), std::size_t{0});
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#include "heavymp_camera.inl"
#include "heavymp_shadows.inl"
#include "heavymp_traffic_runtime.inl"
void drawTractors(Ptr actor, Ptr context) {
    if (!enabled || faulted)
        return;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!actor || !readable(world, 0x35bc) || field<Ptr>(world, 0x31b0) != actor ||
        field<unsigned>(world, 0x35b8) != 0)
        return;
    if (!readable(context, 0xa4))
        return;
    const auto mask = field<std::uint64_t>(context, 0x18);
    if (!mask)
        return;
    for (auto* group : {&trucks, &trailers})
        for (auto& t : *group) {
            if (!t.attached || t.world != world || (t.trailer && trailerFault) ||
                !readable(t.physics, t.trailer ? 0x12b0 : 0x1408))
                continue;
            const Ptr table = field<Ptr>(t.physics, 0);
            if (!readable(table, 0x308) ||
                field<Ptr>(table, 0x300) != engine + (t.trailer ? 0x62cd40 : 0x63baa0))
                continue;
            const auto modelCount = field<std::size_t>(t.physics, 0x168);
            if (!modelCount || modelCount > 256 || !readable(field<Ptr>(t.physics, 0x160), modelCount * 8))
                continue;
            if (mask & shadowPass) {
                if (t.visual.shadows() && shadowReady && !shadowFault)
                    guardedShadow(t, context);
                continue;
            }
            if (!t.visual.shadows() && (mask & sunShadowPasses) && !(mask & ~sunShadowPasses))
                continue;
            // Verified against HaulMP 0x16b762 and ETS2 0x784274:
            // (truck physics, render context, accessory variant, LOD index).
            // Use stock exterior variant and highest detail for the two-player V1.
            bool accepted = true;
            if (t.trailer) {
                accepted = guardedTrailerDraw(t.physics, context);
                if (!accepted) {
                    trailerFault = true;
                    log("[HeavyMP trailer] Native draw fault; trailer sync disabled, tractor drawing "
                        "preserved.");
                }
            } else
                accepted = method<unsigned char>(t.physics, 0x300, context, static_cast<unsigned char>(0),
                                                 std::size_t{0}) != 0;
            if (!t.drawCalls) {
                char message[240]{};
                sprintf_s(message,
                          "[HeavyMP native 0.6.0] First native draw call: pass=%llx accepted=%u "
                          "update-thread=%lu render-thread=%lu; visual verification required.",
                          mask, accepted ? 1u : 0u, ownerThread, GetCurrentThreadId());
                log(message);
            }
            ++t.drawCalls;
            if (accepted)
                ++t.drawAccepted;
            if (mask & sunShadowPasses) {
                if (!t.sunShadowCalls)
                    log("[HeavyMP shadow] Solar geometry pass submitted, including native bit 59; in-game "
                        "confirmation required.");
                ++t.sunShadowCalls;
            }
        }
    guardedTrafficDraw(context);
}
bool guardedDraw(Ptr actor, Ptr context) {
    __try {
        drawTractors(actor, context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void actorDraw(Ptr actor, Ptr context) {
    if (shadowContext == context)
        shadowSubmitted = true;
    originalActorDraw(actor, context);
    // Unlike the SDK update, native rendering can run on a render worker.
    // This callback is the actual engine draw phase; protect ownership with
    // the same lock as creation/removal rather than rejecting another thread.
    lockNative();
    const auto start = heavyMpPerfClock();
    if (!guardedDraw(actor, context)) {
        faulted = true;
        visible = 0;
        log("[HeavyMP native 0.6.0] Native draw fault; bridge stopped. Restart ETS2.");
    }
    perfDraw.add(heavyMpPerfClock() - start);
    ReleaseSRWLockExclusive(&nativeLock);
}
void worldShadowPhase(Ptr world, Ptr context, WorldDraw original) {
    const Ptr previous = shadowContext;
    const bool submitted = shadowSubmitted;
    shadowContext = context;
    shadowSubmitted = false;
    __try {
        original(world, context);
        if (!shadowSubmitted && shadowReady && !shadowFault && enabled && !faulted &&
            world == field<Ptr>(engine, 0x3714fb0) && readable(context, 0xa4) && readable(world, 0x35bc) &&
            field<unsigned>(world, 0x35b8) == 0) {
            const auto mask = field<std::uint64_t>(context, 0x18);
            if (mask & (shadowPass | sunShadowPasses)) {
                lockNative();
                const auto start = heavyMpPerfClock();
                if (!guardedDraw(field<Ptr>(world, 0x31b0), context)) {
                    shadowFault = true;
                    log("[HeavyMP shadow] World shadow pass fault; colour drawing preserved.");
                }
                perfDraw.add(heavyMpPerfClock() - start);
                ReleaseSRWLockExclusive(&nativeLock);
                shadowSubmitted = true;
            }
        }
    } __finally {
        const bool done = shadowSubmitted;
        shadowContext = previous;
        shadowSubmitted = submitted || (previous == context && done);
    }
}
void worldDrawNormal(Ptr world, Ptr context) {
    worldShadowPhase(world, context, originalWorldDraw[0]);
}
void worldDrawShadow(Ptr world, Ptr context) {
    worldShadowPhase(world, context, originalWorldDraw[1]);
}
bool attachShadows() {
    // Optional pass hooks also cover shadows when the user's own truck is
    // excluded. Failure never prevents the established colour renderer.
    if (field<std::uint64_t>(engine, 0x4df8e0) != 0x83485708245c8948ULL ||
        field<std::uint64_t>(engine, 0x4df970) != 0x83485708245c8948ULL ||
        field<std::uint64_t>(engine, 0x16166f0) != 0x74894808245c8948ULL ||
        field<std::uint64_t>(engine, 0x16166b0) != 0x8b4820ec83485340ULL)
        return false;
    constexpr Ptr rvas[] = {0x4df8e0, 0x4df970};
    void* callbacks[] = {reinterpret_cast<void*>(&worldDrawNormal),
                         reinterpret_cast<void*>(&worldDrawShadow)};
    bool good = true;
    for (unsigned i = 0; i < 2 && good; ++i) {
        worldDrawAddresses[i] = reinterpret_cast<void*>(engine + rvas[i]);
        good = (originalWorldDraw[i] ||
                MH_CreateHook(worldDrawAddresses[i], callbacks[i],
                              reinterpret_cast<void**>(&originalWorldDraw[i])) == MH_OK) &&
               MH_EnableHook(worldDrawAddresses[i]) == MH_OK;
    }
    if (!good)
        for (unsigned i = 0; i < 2; ++i)
            if (worldDrawAddresses[i]) {
                MH_DisableHook(worldDrawAddresses[i]);
                MH_RemoveHook(worldDrawAddresses[i]);
                worldDrawAddresses[i] = nullptr;
                originalWorldDraw[i] = nullptr;
            }
    return good;
}
#include "heavymp_map.inc"
bool attachGeometrySubmission() {
    // In addition to the disk fingerprint, refuse an entry already patched by
    // another plugin; the callback ABI is verified only for this exact function.
    if (field<std::uint64_t>(engine, 0x5d8520) != 0x57565518245c8948ULL ||
        field<std::uint64_t>(engine, 0x784170) != 0x8348572024748948ULL ||
        field<std::uint64_t>(engine, 0x856770) != 0x5641544156535540ULL)
        return false;
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    actorUpdateAddress = reinterpret_cast<void*>(engine + 0x5d8520);
    if (!originalActorUpdate && MH_CreateHook(actorUpdateAddress, reinterpret_cast<void*>(&actorUpdate),
                                              reinterpret_cast<void**>(&originalActorUpdate)) != MH_OK)
        return false;
    actorDrawAddress = reinterpret_cast<void*>(engine + 0x784170);
    wheelUpdateAddress = reinterpret_cast<void*>(engine + 0x856770);
    if (!originalActorDraw && MH_CreateHook(actorDrawAddress, reinterpret_cast<void*>(&actorDraw),
                                            reinterpret_cast<void**>(&originalActorDraw)) != MH_OK)
        return false;
    if (!originalWheelUpdate && MH_CreateHook(wheelUpdateAddress, reinterpret_cast<void*>(&wheelUpdate),
                                              reinterpret_cast<void**>(&originalWheelUpdate)) != MH_OK)
        return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&actorUpdate), &module))
        return false;
    if (MH_EnableHook(actorUpdateAddress) != MH_OK)
        return false;
    if (MH_EnableHook(actorDrawAddress) != MH_OK) {
        MH_DisableHook(actorUpdateAddress);
        return false;
    }
    if (MH_EnableHook(wheelUpdateAddress) == MH_OK)
        return true;
    MH_DisableHook(actorDrawAddress);
    MH_DisableHook(actorUpdateAddress);
    return false;
}
} // namespace
bool heavymp_native_camera(HeavyMpTagCamera* camera) {
    AcquireSRWLockShared(&nativeLock);
    const bool valid = guardedCameraSnapshot(camera);
    ReleaseSRWLockShared(&nativeLock);
    return valid;
}
bool heavymp_native_start(scs_log_t logFunction) {
    AcquireSRWLockExclusive(&dispatchLiveLock);
    dispatchLiveEligibleAt = 0;
    dispatchLiveQueued = false;
    dispatchLiveDone = 0;
    dispatchLiveDoneId = {};
    dispatchLiveFailure = 0;
    ReleaseSRWLockExclusive(&dispatchLiveLock);
    collisionUntil = 0;
    collisionApiReady = false;
    tractorCollisionApiReady = false;
    nextTractorCreation = nextTrailerCreation = 0;
    logger = logFunction;
    engine = reinterpret_cast<Ptr>(GetModuleHandleW(nullptr));
    visible = 0;
    ownerThread = 0;
    faulted = false;
    enabled = false;
    localSirenLatch.reset();
    localSirenControlled = false;
    localSirenFault = false;
    if (!executableMatches()) {
        log("[HeavyMP native] Executable fingerprint unsupported; native bridge inactive.");
        return false;
    }
    if (field<Ptr>(engine, 0x1e50508) != engine + 0x838680 ||
        field<Ptr>(engine, 0x1e55cf8) != engine + 0x974820 ||
        field<Ptr>(engine, 0x2332578 + 0x60) != engine + 0x84f260 ||
        field<Ptr>(engine, 0x2332578 + 0x110) != engine + 0x641e90 ||
        field<Ptr>(engine, 0x2332578 + 0xa0) != engine + 0x63b4a0 ||
        field<Ptr>(engine, 0x2332578 + 0xa8) != engine + 0x976090 ||
        field<Ptr>(engine, 0x2332578 + 0x300) != engine + 0x63baa0) {
        log("[HeavyMP native] Engine entry points changed by another plugin; native bridge inactive.");
        return false;
    }
    if (!attachGeometrySubmission()) {
        log("[HeavyMP native 0.6.0] Actor update/draw entry unavailable; native bridge inactive.");
        return false;
    }
    collisionApiReady = field<Ptr>(engine, 0x249ec48 + 8) == engine + 0x1672c50 &&
                        field<Ptr>(engine, 0x249ec48 + 16) == engine + 0x1672cc0;
    log(collisionApiReady ? "[HeavyMP collisions] Native contact API verified; awaiting server policy."
                          : "[HeavyMP collisions] Contact API mismatch; keeping ghost mode.");
    constexpr unsigned char ghostSignature[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
    tractorCollisionApiReady =
        collisionApiReady && field<Ptr>(engine, 0x249ec48 + 0x30) == engine + 0x168a280 &&
        field<Ptr>(engine, 0x249ec48 + 0xd8) == engine + 0x1672df0 &&
        field<Ptr>(engine, 0x2332578 + 0x198) == engine + 0x63ea70 &&
        !memcmp(reinterpret_cast<void*>(engine + 0x63ea70), ghostSignature, sizeof(ghostSignature));
    log(tractorCollisionApiReady ? "[HeavyMP collisions] Tractor native ghost API verified: chassis and "
                                   "cabin contacts controlled together."
                                 : "[HeavyMP collisions] Tractor ghost API unavailable; tractor remains "
                                   "ghost, trailer policy unchanged.");
    if (tractorCollisionApiReady)
        log("[HeavyMP collisions] Remote tractor/cabin contact category 8 enabled; local self-collision "
            "matrix preserved.");
    appearanceReady = true;
    constexpr Ptr constructors[] = {0, 0x8e2cb0, 0xa291c0, 0x918a20, 0x8ee260, 0x8e2560, 0xa28fe0};
    for (unsigned kind = 1; kind <= 6; ++kind)
        if (field<Ptr>(engine, appearanceDescriptors[kind] + 8) != engine + constructors[kind])
            appearanceReady = false;
    if (!appearanceReady)
        log("[HeavyMP appearance] Accessory factory mismatch; configuration sync unavailable, motion bridge "
            "preserved.");
    trailerReady = appearanceReady && field<Ptr>(engine, 0x1e50fd8) == engine + 0x870120 &&
                   field<Ptr>(engine, 0x1e4c2b8) == engine + 0x62a330 &&
                   field<Ptr>(engine, 0x22d4b58 + 0x300) == engine + 0x62cd40 &&
                   field<Ptr>(engine, 0x22d4b58 + 0x328) == engine + 0x62baa0;
    trailerFault = false;
    if (!trailerReady)
        log("[HeavyMP trailer] Native trailer signatures unavailable; tractor bridge preserved.");
    cargoReady = trailerReady && field<Ptr>(engine, 0x1e55548) == engine + 0x940de0;
    if (!cargoReady)
        log("[HeavyMP cargo] Cargo factory unavailable; tractor and trailer bridges preserved.");
    mapFault = false;
    mapCount = 0;
    mapReady = attachMap();
    log(mapReady ? "[HeavyMP map] Native map/GPS/route advisor markers enabled; visual verification required."
                 : "[HeavyMP map] Map signatures unavailable; vehicle bridge preserved.");
    environmentFault = false;
    environmentReady = attachEnvironment();
    log(environmentReady ? "[HeavyMP world] Environment hooks enabled; in-game verification required."
                         : "[HeavyMP world] Environment signatures unavailable; vehicle bridge preserved.");
    hornFault = false;
    hornReady = validateHorns();
    log(hornReady ? "[HeavyMP horn] Native positional horn APIs enabled; in-game verification required."
                  : "[HeavyMP horn] Audio signatures unavailable; vehicle bridge preserved.");
    wiperReady = validateWipers();
    log(wiperReady ? "[HeavyMP wipers] Native exterior animation enabled."
                   : "[HeavyMP wipers] Signatures unavailable; animation disabled.");
    engineAudioFault = false;
    engineAudioReady = validateEngineAudio();
    log(engineAudioReady ? "[HeavyMP audio] Native engine parameters and positional events enabled."
                         : "[HeavyMP audio] Engine signatures unavailable; engine sound disabled.");
    shadowFault = false;
    shadowReady = attachShadows();
    log(shadowReady ? "[HeavyMP shadow] Native shadow-only and world shadow passes enabled; visual "
                      "verification required."
                    : "[HeavyMP shadow] Shadow pass hooks unavailable; vehicle drawing preserved.");
    trafficRole = trafficEpoch = trafficControlSequence = trafficSnapshotSequence = 0;
    trafficControlAt = trafficIncomingAt = trafficCaptureAt = 0;
    trafficWorld = trafficManager = 0;
    trafficAppliedRole = ~0u;
    trafficAppliedLimit = 0;
    trafficFault = false;
    trafficDensityFault = false;
    trafficCaptureCoherent = false;
    trafficWorkCursor = 0;
    trafficModelFailures.clear();
    trafficCaptured.clear();
    trafficIncoming.clear();
    trafficVisuals.clear();
    trafficApiReady = appearanceReady && field<Ptr>(engine, 0x1e5a218) == engine + 0xa2cfa0 &&
                      field<Ptr>(engine, 0x2416be8 + 0x78) == engine + 0x12e9940 &&
                      field<Ptr>(engine, 0x2416be8 + 0x38) == engine + 0x12ec9a0 &&
                      field<Ptr>(engine, 0x1e5a2c8) == engine + 0xa315f0 &&
                      field<Ptr>(engine, 0x2417018 + 0x78) == engine + 0x12ee9f0;
    enabled = true;
    log("[HeavyMP native] Development bridge enabled for validated ETS2 1.62.3.3 executable.");
    return true;
}
void heavymp_native_frame(const HeavyMpNativePeer* peers, unsigned count, bool active) {
    if (!enabled || faulted)
        return;
    lockNative();
    const auto start = heavyMpPerfClock();
    frameCadence.record(heavyMpPerfMs(start), active);
    guardedEnvironmentTick();
    visualCameraValid = guardedCameraSnapshot(&visualCamera);
    loadBudget.reset(workClock());
    if (!guardedTick(peers, std::min(count, 32u), active)) {
        for (auto& t : trucks)
            guardedFreeHorns(t);
        faulted = true;
        visible = 0;
        log("[HeavyMP native] Engine call fault; bridge stopped. Restart ETS2 before further testing.");
    }
    if (!guardedTrailers(peers, std::min(count, 32u), active)) {
        trailerFault = true;
        log("[HeavyMP trailer] Trailer update fault; trailer sync disabled, tractor bridge preserved.");
    }
    guardedTrafficTick(active);
    const auto actors = trucks.size() + trailers.size();
    perfFrame.add(heavyMpPerfClock() - start);
    ReleaseSRWLockExclusive(&nativeLock);
    static ULONGLONG reportAt = GetTickCount64();
    const auto now = GetTickCount64();
    if (now - reportAt >= 5000) {
        const auto window = now - reportAt;
        reportAt = now;
        const auto cadence = frameCadence.take();
        if (cadence.count) {
            char line[280]{};
            sprintf_s(line,
                      "[HeavyMP performance] phase=sdk-cadence samples=%u p50-ms=%.3f p95-ms=%.3f "
                      "p99-ms=%.3f max-ms=%.3f over-16.67ms=%u target=60FPS not-GPU-present",
                      cadence.count, cadence.p50, cadence.p95, cadence.p99, cadence.maximum,
                      cadence.overBudget);
            log(line);
        }
        const char* names[] = {"update", "wheels", "models", "draw", "contacts", "lock-wait"};
        HeavyMpPerfCounter* counters[] = {&perfFrame, &perfWheels,   &perfModels,
                                          &perfDraw,  &perfContacts, &perfWait};
        for (unsigned i = 0; i < 6; ++i) {
            const auto sample = counters[i]->take();
            if (!sample.calls)
                continue;
            char message[256]{};
            sprintf_s(message,
                      "[HeavyMP performance] phase=%s actors=%zu calls=%llu cpu-ms-total=%.3f avg=%.3f "
                      "max=%.3f window-ms=%llu",
                      names[i], actors, sample.calls, heavyMpPerfMs(sample.ticks),
                      heavyMpPerfMs(sample.ticks) / sample.calls, heavyMpPerfMs(sample.peak), window);
            log(message);
        }
    }
}
void heavymp_native_stop() {
    AcquireSRWLockExclusive(&dispatchLiveLock);
    dispatchLiveEligibleAt = 0;
    dispatchLiveQueued = false;
    dispatchLiveDone = 0;
    ReleaseSRWLockExclusive(&dispatchLiveLock);
    collisionUntil = 0;
    shadowReady = false;
    for (const auto address : worldDrawAddresses)
        if (address)
            MH_DisableHook(address);
    {
        AcquireSRWLockExclusive(&worldLock);
        sharedWorld.active = false;
        ReleaseSRWLockExclusive(&worldLock);
    }
    guardedEnvironmentTick();
    environmentReady = false;
    worldEconomy = 0;
    for (auto& h : environmentHooks)
        if (h) {
            MH_DisableHook(h);
            MH_RemoveHook(h);
            h = nullptr;
        }
    if (mapDrawAddress)
        MH_DisableHook(mapDrawAddress);
    if (mapIconsAddress)
        MH_DisableHook(mapIconsAddress);
    if (wheelUpdateAddress)
        MH_DisableHook(wheelUpdateAddress);
    if (actorDrawAddress)
        MH_DisableHook(actorDrawAddress);
    if (actorUpdateAddress)
        MH_DisableHook(actorUpdateAddress);
    lockNative();
    if (mapInput != INVALID_SOCKET) {
        closesocket(mapInput);
        mapInput = INVALID_SOCKET;
    }
    mapCount = 0;
    mapReady = false;
    if (enabled && !faulted && (!ownerThread || ownerThread == GetCurrentThreadId())) {
        trafficRole = 0;
        guardedTrafficTick(false);
        guardedTrailers(nullptr, 0, false);
        guardedTick(nullptr, 0, false);
    }
    trafficApiReady = false;
    trafficControlAt = 0;
    trafficCaptured.clear();
    trafficIncoming.clear();
    guardedFreeLocalCarAudio();
    guardedFreeLocalSiren();
    escortBeaconActive = false;
    localPanelActive = false;
    localPanelFault = false;
    localPanelRig = {};
    enabled = false;
    visible = 0;
    ReleaseSRWLockExclusive(&nativeLock);
}
unsigned heavymp_native_count() {
    return visible.load();
}
bool heavymp_native_active() {
    return enabled && !faulted;
}
void heavymp_native_traffic_packet(const unsigned char* data, size_t bytes) {
    if (!enabled || faulted)
        return;
    AcquireSRWLockExclusive(&nativeLock);
    trafficPacket(data, bytes);
    ReleaseSRWLockExclusive(&nativeLock);
}
bool heavymp_native_traffic_json(std::string* output) {
    if (!output || !enabled || faulted)
        return false;
    AcquireSRWLockShared(&nativeLock);
    const bool active = trafficControlAt != 0;
    if (active)
        *output = trafficJson();
    ReleaseSRWLockShared(&nativeLock);
    return active;
}
bool heavymp_native_local_lights(unsigned* lights) {
    if (!enabled || faulted)
        return false;
    AcquireSRWLockShared(&nativeLock);
    const bool result = guardedLocalLights(lights);
    if (result) {
        *lights &= ~heavyMpPanelLightBit;
        if (localPanelActive && escortBeaconActive.load())
            *lights |= heavyMpPanelLightBit;
    }
    ReleaseSRWLockShared(&nativeLock);
    return result;
}
void heavymp_native_collision_packet(const unsigned char* p, size_t n) {
    bool collide = false;
    if (!heavyMpCollisionPacket(p, n, &collide))
        return;
    collisionUntil = collide ? GetTickCount64() + 1500 : 0;
}
void heavymp_native_world_packet(const unsigned char* p, size_t n) {
    receiveWorld(p, n);
}
bool heavymp_native_local_horns(unsigned* horns) {
    if (!horns || !enabled || faulted || !hornReady)
        return false;
    const bool valid = guardedLocalHorns(horns);
    static unsigned previous = ~0u;
    if (valid && localSirenControlled)
        *horns = (*horns & 1u) | (localSirenLatch.on ? 2u : 0u);
    if (valid && previous != *horns) {
        previous = *horns;
        char line[100]{};
        sprintf_s(line, "[HeavyMP horn] Local mapped state: normal=%u air=%u", *horns & 1, (*horns >> 1) & 1);
        log(line);
    }
    return valid;
}
bool heavymp_native_local_appearance(HeavyMpAppearance* appearance) {
    if (!enabled || faulted || !appearanceReady)
        return false;
    AcquireSRWLockShared(&nativeLock);
    const bool result = guardedLocalAppearance(appearance);
    ReleaseSRWLockShared(&nativeLock);
    return result;
}

bool heavymp_native_local_trailer_appearance(HeavyMpAppearance* appearance, unsigned segment) {
    if (!enabled || faulted || !trailerReady)
        return false;
    AcquireSRWLockShared(&nativeLock);
    const bool result = guardedLocalTrailerAppearance(appearance, segment);
    ReleaseSRWLockShared(&nativeLock);
    return result;
}
unsigned heavymp_native_local_trailer_count() {
    if (!enabled || faulted || !trailerReady)
        return 0;
    unsigned count = 0;
    AcquireSRWLockShared(&nativeLock);
    __try {
        while (count < 10 && localTrailerPhysics(count))
            ++count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = 0;
    }
    ReleaseSRWLockShared(&nativeLock);
    return count;
}

bool heavymp_native_local_wipers(float* phase, float* rear, unsigned* mode) {
    AcquireSRWLockShared(&nativeLock);
    const bool result = guardedLocalWipers(phase, rear, mode);
    ReleaseSRWLockShared(&nativeLock);
    return result;
}

bool heavymp_native_local_audio(std::array<std::string, 3>* refs) {
    AcquireSRWLockShared(&nativeLock);
    const bool result = guardedLocalAudio(refs);
    ReleaseSRWLockShared(&nativeLock);
    return result;
}
void heavymp_native_car_audio(const scs_value_dplacement_t& p, float rpm, float throttle, float speed,
                              bool running, bool driving) {
    guardedCarEngineFrame(p, rpm, throttle, speed, running, driving);
}
void heavymp_native_siren_frame(const scs_value_dplacement_t& p, float speed, bool driving) {
    guardedLocalSirenFrame(p, speed, driving);
}

void heavymp_native_repair_packet(const unsigned char* packet, size_t bytes) {
    if (!enabled || faulted || bytes != 32 || memcmp(packet, "HMR1", 4))
        return;
    unsigned version = 0;
    memcpy(&version, packet + 4, 4);
    if (version != 1)
        return;
    AcquireSRWLockExclusive(&repairLock);
    if (memcmp(repairId.data(), packet + 16, 16) != 0) {
        memcpy(repairId.data(), packet + 16, 16);
        repairReceived = GetTickCount64();
        repairCompleted = 0;
        repairPending = true;
    }
    ReleaseSRWLockExclusive(&repairLock);
}
bool heavymp_native_repair_result(char* id, bool* okay, const char** reason) {
    AcquireSRWLockShared(&repairLock);
    const bool ready = repairCompleted && GetTickCount64() - repairCompleted < 14000;
    if (ready) {
        unsigned at = 0;
        for (unsigned i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                id[at++] = '-';
            sprintf_s(id + at, 37 - at, "%02x", repairId[i]);
            at += 2;
        }
        id[36] = 0;
        *okay = repairOkay;
        *reason = repairFailureReason();
    }
    ReleaseSRWLockShared(&repairLock);
    return ready;
}

// Retained ABI for older callers; the retired sign has no frame work.
void heavymp_native_panel_frame(bool) {}

void heavymp_native_dispatch_eligible(bool value) {
    dispatchLiveEligible(value);
}
void heavymp_native_dispatch_packet(const unsigned char* packet, size_t bytes) {
    dispatchLivePacket(packet, bytes);
}
bool heavymp_native_dispatch_result(char* id, bool* okay, const char** reason, const char** market,
                                    unsigned* units) {
    return dispatchLiveResult(id, okay, reason, market, units);
}

void heavymp_native_dispatch_run() {
    if (enabled && !faulted && ownerThread == GetCurrentThreadId())
        processLiveFreight();
}
bool heavymp_native_dispatch_unavailable(std::string* out) {
    __try {
        return buildLiveCompanyExclusions(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
