// Included in the fingerprinted native bridge namespace. All engine operations
// run on the SDK owner thread; rendering uses the existing nativeLock.
#include "heavymp_traffic_lease.h"
#include "heavymp_traffic_population.h"
#include <limits>
struct TrafficVisual {
    unsigned id = 0;
    Ptr definition = 0, physics = 0, world = 0;
    ULONGLONG created = 0, pollStarted = 0;
    bool ready = false;
    const char* failureStage = "create";
    HeavyMpTrafficSample sample;
    HeavyMpTrafficMotion motion;
};
std::vector<TrafficVisual> trafficVisuals;
std::vector<HeavyMpTrafficSample> trafficIncoming, trafficCaptured;
unsigned trafficRole = 0, trafficEpoch = 0, trafficLimit = 8, trafficControlSequence = 0,
         trafficSnapshotSequence = 0;
ULONGLONG trafficControlAt = 0, trafficIncomingAt = 0, trafficCaptureAt = 0;
bool trafficApiReady = false, trafficFault = false, trafficCaptureCoherent = false;
Ptr trafficWorld = 0, trafficManager = 0;
unsigned trafficAppliedRole = ~0u, trafficAppliedLimit = 0;
size_t trafficWorkCursor = 0;
unsigned long long trafficDrawCalls = 0;
size_t trafficNativeCount = 0;
struct TrafficModelFailure {
    std::string model;
    ULONGLONG until = 0;
};
std::vector<TrafficModelFailure> trafficModelFailures;
bool trafficDensityFault = false;
std::string trafficModelIdentity(const HeavyMpTrafficSample& sample) {
    std::string result = std::to_string(sample.vehicleType);
    result.append(reinterpret_cast<const char*>(&sample.lookSeed), sizeof(sample.lookSeed));
    result += sample.hasPaintColor ? '1' : '0';
    if (sample.hasPaintColor)
        result.append(reinterpret_cast<const char*>(sample.paintColor.data()), 12);
    for (const auto& part : sample.accessories) {
        result += std::to_string(part.kind);
        result += ':';
        result += part.path;
        result += '\n';
        result.append(reinterpret_cast<const char*>(&part.flags), sizeof(part.flags));
        result.append(reinterpret_cast<const char*>(part.colors.data()),
                      (part.kind == 4 ? 18u : 3u) * sizeof(float));
    }
    return result;
}
void trafficModelFailed(const HeavyMpTrafficSample& sample, ULONGLONG now, const char* reason,
                        unsigned flags = 0) {
    const auto key = trafficModelIdentity(sample);
    for (auto& f : trafficModelFailures)
        if (f.model == key) {
            f.until = now + 30000;
            return;
        }
    if (trafficModelFailures.size() >= 64)
        trafficModelFailures.erase(trafficModelFailures.begin());
    trafficModelFailures.push_back({key, now + 30000});
    char line[640]{};
    sprintf_s(line,
              "[HeavyMP traffic] Model failed: id=%u type=%s stage=%s flags=%08x asset=%s; retry delayed 30 "
              "seconds.",
              sample.nativeId, sample.vehicleType == 255 ? "trailer" : "vehicle", reason, flags,
              sample.accessories.empty() ? "none" : sample.accessories[0].path.c_str());
    log(line);
}
bool trafficSameModel(const HeavyMpTrafficSample& a, const HeavyMpTrafficSample& b) {
    if (a.vehicleType != b.vehicleType || a.lookSeed != b.lookSeed || a.hasPaintColor != b.hasPaintColor ||
        (a.hasPaintColor && a.paintColor != b.paintColor) || a.accessories.size() != b.accessories.size())
        return false;
    for (size_t i = 0; i < a.accessories.size(); i++)
        if (a.accessories[i].kind != b.accessories[i].kind ||
            a.accessories[i].path != b.accessories[i].path ||
            a.accessories[i].flags != b.accessories[i].flags ||
            a.accessories[i].colors != b.accessories[i].colors)
            return false;
    return true;
}
void trafficFree(TrafficVisual& v, Ptr current) {
    v.ready = false;
    if (v.world == current) {
        if (v.physics) {
            call<void>(0x12de760, v.physics, static_cast<unsigned char>(0), static_cast<unsigned char>(0));
            method<void>(v.physics, 0, static_cast<unsigned>(1));
        }
        release(v.definition);
    }
    v.physics = v.definition = 0;
}
bool trafficCreate(TrafficVisual& v) {
    const bool trailer = v.sample.vehicleType == 255;
    v.failureStage = "definition_factory";
    v.definition = factory(trailer ? 0x1e5a2c0 : 0x1e5a210);
    if (!v.definition)
        return false;
    for (const auto& part : v.sample.accessories) {
        v.failureStage = "accessory_factory";
        Ptr item = factory(part.kind == 1   ? 0x1e53260
                           : part.kind == 3 ? 0x1e54160
                           : part.kind == 4 ? 0x1e53540
                                            : 0x1e53210);
        if (!item)
            return false;
        if (!string(item + 0x10, part.path.c_str())) {
            release(item);
            return false;
        }
        if (part.kind == 3) {
            field<unsigned>(item, 0x40) = part.flags;
            memcpy(reinterpret_cast<void*>(item + 0x44), part.colors.data(), 12);
        }
        if (part.kind == 4)
            memcpy(reinterpret_cast<void*>(item + 0x40), part.colors.data(), 72);
        const auto count = field<size_t>(v.definition, 0x20);
        if (count >= 16 || !method<bool>(v.definition + 0x10, 0x18, count + 1)) {
            release(item);
            return false;
        }
        field<Ptr>(field<Ptr>(v.definition, 0x18), count * 8) = item;
        field<size_t>(v.definition, 0x20) = count + 1;
    }
    // Pin the source's selected linear RGB in a one-colour palette. A synthesized
    // definition otherwise falls back to the receiver's unrelated default palette.
    if (!trailer && v.sample.hasPaintColor) {
        v.failureStage = "paint_palette";
        const Ptr palette = v.definition + 0x1f8;
        if (field<Ptr>(palette, 0) != engine + 0x2252190 || !method<bool>(palette, 0x18, size_t{1}))
            return false;
        const Ptr colors = field<Ptr>(palette, 8);
        if (!readable(colors, 12))
            return false;
        memcpy(reinterpret_cast<void*>(colors), v.sample.paintColor.data(), 12);
        field<size_t>(palette, 16) = 1;
    }
    v.failureStage = "physics_allocation";
    const Ptr allocation = call<Ptr>(0x1d53bc8, size_t{trailer ? 0x238u : 0x328u});
    if (!allocation)
        return false;
    v.physics = call<Ptr>(trailer ? 0x12ee4a0 : 0x12e9310, allocation, Ptr{0});
    if (!readable(v.physics, trailer ? 0x238 : 0x328))
        return false;
    InterlockedIncrement(reinterpret_cast<LONG*>(v.definition + 8));
    field<Ptr>(v.physics, trailer ? 0x228 : 0x310) = v.definition;
    field<float>(v.physics, 0x1c4) = v.sample.lookSeed;
    field<unsigned>(v.physics, 0x1b8) = 0x2000; // Visual replicas: no dynamic contact bodies.
    call<void>(0x12c08b0, v.physics, v.definition, static_cast<unsigned char>(1),
               static_cast<unsigned char>(0));
    return true;
}
void trafficDensity(Ptr manager, unsigned role, unsigned limit) {
    if (manager == trafficManager && role == trafficAppliedRole && limit == trafficAppliedLimit)
        return;
    if (!readable(manager, 0x600) || field<Ptr>(manager, 0) != engine + 0x240b190)
        return;
    const Ptr data = field<Ptr>(manager, 0x18);
    if (!readable(data, 0x14))
        return;
    const auto base = field<unsigned>(data, 0x10);
    if (!base || base > 4096)
        return;
    const float density = heavyMpTrafficDensityMultiplier(base, limit, role == 2);
    // Exact entry called by the verified g_traffic configuration callback.
    // It adjusts the native population and removes excess actors safely.
    call<void>(0x1252880, manager, density);
    trafficManager = manager;
    trafficAppliedRole = role;
    trafficAppliedLimit = limit;
    char line[180]{};
    sprintf_s(line, "[HeavyMP traffic] Native authority role=%u limit=%u population=%u.", role, limit,
              field<unsigned>(manager, 0x594));
    log(line);
}
bool trafficSafeDensity(unsigned role) {
    if (trafficDensityFault)
        return false;
    __try {
        trafficDensity(field<Ptr>(engine, 0x3715000), role, trafficLimit);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        trafficDensityFault = true;
        return false;
    }
}
void trafficEmergencyStop() {
    trafficFault = true;
    trafficRole = 0;
    trafficCaptured.clear();
    for (auto& v : trafficVisuals)
        v.ready = false;
    // If another operation faulted, the independently guarded population setter
    // can still stop this client's native simulation before a host handoff.
    trafficSafeDensity(0);
}
void trafficTick(bool driving) {
    if (!trafficApiReady || trafficFault)
        return;
    const auto now = GetTickCount64();
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    const bool active = trafficControlAt && now - trafficControlAt <= 1000 && readable(world, 0x31b8) &&
                        field<Ptr>(world, 0x31b0);
    const unsigned role = active ? trafficRole : 0;
    if (trafficWorld != world) {
        for (auto& v : trafficVisuals)
            trafficFree(v, world);
        trafficVisuals.clear();
        trafficIncoming.clear();
        trafficCaptured.clear();
        trafficWorld = world;
        trafficManager = 0;
        trafficAppliedRole = ~0u;
    }
    // Never touch solo traffic if no server has activated this feature.
    if (trafficControlAt && !trafficSafeDensity(role)) {
        trafficEmergencyStop();
        log("[HeavyMP traffic] Population API fault; traffic disabled for this client.");
        return;
    }
    if (role != 1 || !driving || now - trafficIncomingAt > 1500) {
        for (auto& v : trafficVisuals)
            trafficFree(v, world);
        trafficVisuals.clear();
    } else {
        for (auto it = trafficVisuals.begin(); it != trafficVisuals.end();) {
            const auto found = std::find_if(trafficIncoming.begin(), trafficIncoming.end(),
                                            [&](const auto& s) { return s.nativeId == it->id; });
            if (found == trafficIncoming.end() || !trafficSameModel(*found, it->sample)) {
                trafficFree(*it, world);
                it = trafficVisuals.erase(it);
            } else
                ++it;
        }
        const size_t first = trafficIncoming.empty() ? 0 : trafficWorkCursor % trafficIncoming.size();
        trafficWorkCursor += 4;
        for (size_t step = 0; step < trafficIncoming.size(); step++) {
            const auto& sample = trafficIncoming[(first + step) % trafficIncoming.size()];
            auto found = std::find_if(trafficVisuals.begin(), trafficVisuals.end(),
                                      [&](const auto& v) { return v.id == sample.nativeId; });
            if (found == trafficVisuals.end()) {
                const auto key = trafficModelIdentity(sample);
                if (std::any_of(trafficModelFailures.begin(), trafficModelFailures.end(),
                                [&](const auto& f) { return f.model == key && now < f.until; }))
                    continue;
                if (trafficVisuals.size() >= trafficLimit * 4 ||
                    !loadBudget.take(HeavyMpLoadKind::Create, workClock()))
                    continue;
                TrafficVisual v;
                v.id = sample.nativeId;
                v.world = world;
                v.sample = sample;
                v.created = now;
                v.motion.push(sample, static_cast<double>(trafficIncomingAt));
                if (!trafficCreate(v)) {
                    trafficModelFailed(sample, now, v.failureStage);
                    trafficFree(v, world);
                    continue;
                }
                trafficVisuals.push_back(std::move(v));
                found = trafficVisuals.end() - 1;
            }
            auto& v = *found;
            v.sample = sample;
            v.motion.push(sample, static_cast<double>(trafficIncomingAt));
            if (!v.ready) {
                if (v.pollStarted && now - v.pollStarted > 10000) {
                    trafficModelFailed(sample, now, "loading_timeout", field<unsigned>(v.physics, 0x1b8));
                    trafficFree(v, world);
                    continue;
                }
                if (!loadBudget.take(HeavyMpLoadKind::Poll, workClock()))
                    continue;
                if (!v.pollStarted)
                    v.pollStarted = now;
                if (!method<unsigned char>(v.physics, 0x78, static_cast<unsigned char>(0),
                                           static_cast<unsigned char>(0))) {
                    trafficModelFailed(sample, now, "native_loader", field<unsigned>(v.physics, 0x1b8));
                    trafficFree(v, world);
                    continue;
                }
                if ((field<unsigned>(v.physics, 0x1b8) & 0xe0000) != 0xe0000)
                    continue;
                if (!readable(field<Ptr>(v.physics, 0x168), 48)) {
                    trafficModelFailed(sample, now, "placement_state", field<unsigned>(v.physics, 0x1b8));
                    trafficFree(v, world);
                    continue;
                }
                call<void>(0x12de760, v.physics, static_cast<unsigned char>(1),
                           static_cast<unsigned char>(0));
                v.ready = true;
                char line[480]{};
                sprintf_s(
                    line, "[HeavyMP traffic] Replica loaded: id=%u type=%s models=%zu seed=%.9g asset=%s.",
                    v.id, sample.vehicleType == 255 ? "trailer" : "vehicle", field<size_t>(v.physics, 0x38),
                    sample.lookSeed, sample.accessories[0].path.c_str());
                log(line);
            }
            HeavyMpTrafficSample rendered;
            if (!v.motion.render(static_cast<double>(now), rendered))
                continue;
            const auto sx = static_cast<short>(floor(rendered.position[0] / 512)),
                       sz = static_cast<short>(floor(rendered.position[2] / 512));
            Placement pose{static_cast<float>(rendered.position[0] - sx * 512),
                           static_cast<float>(rendered.position[1]),
                           static_cast<float>(rendered.position[2] - sz * 512),
                           sx,
                           sz,
                           rendered.quaternion[0],
                           rendered.quaternion[1],
                           rendered.quaternion[2],
                           rendered.quaternion[3]};
            const Ptr state = field<Ptr>(v.physics, 0x168);
            field<unsigned>(state, 0) = 2;
            memcpy(reinterpret_cast<void*>(state + 16), &pose, 32);
            field<unsigned>(v.physics, 0x1c0) = sample.lights;
            method<void>(v.physics, 0x30);
        }
        trafficVisuals.erase(std::remove_if(trafficVisuals.begin(), trafficVisuals.end(),
                                            [](const auto& v) { return !v.physics; }),
                             trafficVisuals.end());
    }
    if (role == 2 && driving && now - trafficCaptureAt >= 100) {
        trafficCaptureAt = now;
        auto capture = heavympCaptureTraffic(
            engine, true,
            [](auto at, void* target, size_t size) {
                if (!readable(at, size))
                    return false;
                memcpy(target, reinterpret_cast<void*>(at), size);
                return true;
            },
            trafficLimit);
        trafficNativeCount = capture.nativeCount;
        trafficCaptureCoherent = capture.coherent;
        trafficCaptured =
            capture.coherent ? std::move(capture.vehicles) : std::vector<HeavyMpTrafficSample>{};
        if (trafficCaptured.size() > trafficLimit * 4)
            trafficCaptured.resize(trafficLimit * 4);
    }
    if (role != 2 || !driving) {
        trafficCaptureCoherent = false;
        trafficCaptured.clear();
    }
}
void trafficDraw(Ptr context) {
    if (trafficApiReady && !trafficFault && trafficRole == 1 && GetTickCount64() - trafficControlAt <= 1000 &&
        GetTickCount64() - trafficIncomingAt <= 1500)
        for (auto& v : trafficVisuals)
            if (v.ready && v.physics) {
                method<void>(v.physics, 0x38, context);
                ++trafficDrawCalls;
            }
}
void guardedTrafficTick(bool driving) {
    __try {
        trafficTick(driving);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        trafficEmergencyStop();
        log("[HeavyMP traffic] Native traffic fault isolated; player synchronization preserved.");
    }
}
void guardedTrafficDraw(Ptr context) {
    __try {
        trafficDraw(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        trafficEmergencyStop();
        log("[HeavyMP traffic] Traffic render fault isolated.");
    }
}
void trafficPacket(const unsigned char* p, size_t n) {
    if (!trafficApiReady || trafficFault || !p || n < 16)
        return;
    using heavympTrafficCaptureDetail::value;
    if (!memcmp(p, "HIA1", 4)) {
        if (n != 24)
            return;
        const unsigned sequence = value<unsigned>(p, 4), role = value<unsigned>(p, 8),
                       epoch = value<unsigned>(p, 12), limit = value<unsigned>(p, 16);
        if (role > 2 || limit < 1 || limit > 0x3fffffff || value<unsigned>(p, 20))
            return;
        if (!heavyMpTrafficSequenceAllowed(trafficControlAt, GetTickCount64(), 1000, sequence,
                                           trafficControlSequence, epoch == trafficEpoch))
            return;
        if (epoch != trafficEpoch) {
            trafficIncoming.clear();
            trafficIncomingAt = 0;
            trafficSnapshotSequence = 0;
        }
        trafficRole = role;
        trafficEpoch = epoch;
        trafficLimit = limit;
        trafficControlSequence = sequence;
        trafficControlAt = GetTickCount64();
        return;
    }
    if (memcmp(p, "HIV1", 4) || trafficRole != 1 || GetTickCount64() - trafficControlAt > 1000)
        return;
    const unsigned sequence = value<unsigned>(p, 4), count = value<unsigned>(p, 8),
                   epoch = value<unsigned>(p, 12);
    if (count > trafficLimit * 4 || epoch != trafficEpoch ||
        !heavyMpTrafficSequenceAllowed(trafficIncomingAt, GetTickCount64(), 1500, sequence,
                                       trafficSnapshotSequence))
        return;
    size_t offset = 16;
    std::vector<HeavyMpTrafficSample> samples;
    std::vector<unsigned> ids;
    for (unsigned i = 0; i < count; i++) {
        if (n - offset < 4)
            return;
        const unsigned length = value<unsigned>(p, offset);
        offset += 4;
        if (length > n - offset)
            return;
        HeavyMpTrafficSample sample;
        std::uint32_t ignored = 0;
        if (!heavympTrafficLabPacket(p + offset, length, sample, ignored) ||
            std::find(ids.begin(), ids.end(), sample.nativeId) != ids.end())
            return;
        ids.push_back(sample.nativeId);
        samples.push_back(std::move(sample));
        offset += length;
    }
    if (offset != n)
        return;
    trafficIncoming = std::move(samples);
    trafficIncomingAt = GetTickCount64();
    trafficSnapshotSequence = sequence;
}
std::string trafficSampleJson(const HeavyMpTrafficSample& s) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(10) << "{\"nativeId\":" << s.nativeId << ",\"vehicleType\":" << s.vehicleType
        << ",\"lights\":" << s.lights << ",\"lookSeed\":" << s.lookSeed << ",\"paintColor\":";
    if (s.hasPaintColor) {
        out << '[' << s.paintColor[0] << ',' << s.paintColor[1] << ',' << s.paintColor[2] << ']';
    } else
        out << "null";
    out << ",\"position\":[";
    for (unsigned j = 0; j < 3; j++) {
        if (j)
            out << ',';
        out << s.position[j];
    }
    out << "],\"quaternion\":[";
    for (unsigned j = 0; j < 4; j++) {
        if (j)
            out << ',';
        out << s.quaternion[j];
    }
    out << "],\"accessories\":[";
    for (size_t j = 0; j < s.accessories.size(); j++) {
        if (j)
            out << ',';
        const auto& p = s.accessories[j];
        out << "{\"kind\":" << p.kind << ",\"path\":\"" << p.path << "\",\"flags\":" << p.flags
            << ",\"colors\":[";
        for (unsigned c = 0; c < (p.kind == 4 ? 18u : 3u); c++) {
            if (c)
                out << ',';
            out << p.colors[c];
        }
        out << "]}";
    }
    return out.str() + "]}";
}
std::string trafficJson() {
    unsigned readyVehicles = 0, readyTrailers = 0;
    for (const auto& v : trafficVisuals)
        if (v.ready) {
            if (v.sample.vehicleType == 255)
                readyTrailers++;
            else
                readyVehicles++;
        }
    std::string result = "{\"protocol\":1,\"gameId\":\"eut2\",\"type\":\"traffic_native\",\"ready\":" +
                         std::string(trafficApiReady && !trafficFault ? "true" : "false") +
                         ",\"captureCoherent\":" + (trafficCaptureCoherent ? "true" : "false") +
                         ",\"epoch\":" + std::to_string(trafficEpoch) +
                         ",\"nativeCount\":" + std::to_string(trafficNativeCount) +
                         ",\"replicas\":" + std::to_string(trafficVisuals.size()) +
                         ",\"readyVehicles\":" + std::to_string(readyVehicles) +
                         ",\"readyTrailers\":" + std::to_string(readyTrailers) +
                         ",\"failedModels\":" + std::to_string(trafficModelFailures.size()) +
                         ",\"samples\":[";
    // A dense stream must remain a usable frame, rather than exceed UDP and vanish.
    bool first = true;
    for (const auto& sample : trafficCaptured) {
        const auto item = trafficSampleJson(sample);
        if (result.size() + item.size() + 3 >= 59000)
            break;
        if (!first)
            result += ',';
        result += item;
        first = false;
    }
    return result + "]}";
}
