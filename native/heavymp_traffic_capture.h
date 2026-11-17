#pragma once
// Development-only, read-only capture for the fingerprint verified in NOTES.md.
// This header creates no actors, installs no hooks and never activates traffic.
// Reader(address, destination, bytes) must return false on inaccessible memory.
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "heavymp_appearance.h"

struct HeavyMpTrafficSample {
    std::uint32_t nativeId = 0, vehicleType = 0, lights = 0;
    float lookSeed = 0;
    bool hasPaintColor = false;
    std::array<float, 3> paintColor{};
    std::array<double, 3> position{};
    std::array<float, 4> quaternion{};
    // Desired speed is intentionally absent: it is not measured vehicle speed.
    std::vector<HeavyMpAccessory> accessories;
};
struct HeavyMpTrafficCapture {
    bool coherent = false;
    std::size_t nativeCount = 0, rejected = 0;
    std::vector<HeavyMpTrafficSample> vehicles;
};
namespace heavympTrafficCaptureDetail {
using Address = std::uint64_t;
inline bool address(Address value, std::size_t bytes) {
    return value >= 0x10000 && bytes && value < 0x7fffffffffffULL && bytes <= 65536 &&
           value + bytes > value && value + bytes < 0x7fffffffffffULL;
}
template <class T, class Reader> bool load(Reader& reader, Address at, T& value) {
    return address(at, sizeof(T)) && reader(at, &value, sizeof(T));
}
template <class T> T value(const unsigned char* data, std::size_t offset) {
    T out;
    std::memcpy(&out, data + offset, sizeof(T));
    return out;
}
struct Placement {
    float x, y, z;
    std::int16_t sx, sz;
    float w, qx, qy, qz;
};
static_assert(sizeof(Placement) == 32);
template <class Reader>
bool paintColor(Reader& reader, Address engine, Address manager, Address definition, float seed,
                std::array<float, 3>& rgb) {
    Address palette = definition + 0x1f8, vtable = 0, array = 0;
    std::uint64_t count = 0, capacity = 0;
    if (!load(reader, palette + 16, count))
        return false;
    if (!count) {
        Address colors = 0;
        if (!load(reader, manager + 0x20, colors) || !colors)
            return false;
        palette = colors + 0x10;
        if (!load(reader, palette + 16, count))
            return false;
    }
    if (!count || !load(reader, palette, vtable) || vtable != engine + 0x2252190 ||
        !load(reader, palette + 8, array) || !load(reader, palette + 24, capacity) || count > capacity ||
        capacity > 65536)
        return false;
    const float selected = static_cast<float>(count - 1) * seed + .5f;
    const auto index = std::min<std::uint64_t>(count - 1, static_cast<std::uint64_t>(std::floor(selected)));
    if (!address(array + index * 12, 12) || !reader(array + index * 12, rgb.data(), 12))
        return false;
    for (auto c : rgb)
        if (!std::isfinite(c) || c < 0 || c > 16)
            return false;
    return true;
}
} // namespace heavympTrafficCaptureDetail
template <class Reader>
HeavyMpTrafficCapture heavympCaptureTraffic(std::uint64_t engine, bool verifiedExecutable, Reader reader,
                                            std::uint32_t maxRoots = 32) {
    using namespace heavympTrafficCaptureDetail;
    HeavyMpTrafficCapture result;
    if (!verifiedExecutable)
        return result;
    Address manager = 0, table = 0, vtable = 0;
    std::uint64_t count = 0, capacity = 0;
    if (!load(reader, engine + 0x3715000, manager) || !load(reader, manager, vtable) ||
        vtable != engine + 0x240b190 || !load(reader, manager + 0xf8, table) ||
        !load(reader, manager + 0x100, count) || !load(reader, manager + 0x108, capacity) ||
        count > capacity || capacity > 0xffffffffULL)
        return result;
    result.nativeCount = static_cast<std::size_t>(count);
    const auto sampled = static_cast<std::size_t>(std::min<std::uint64_t>(count, maxRoots));
    for (std::size_t i = 0; i < sampled; ++i) {
        Address actor = 0, physics = 0, definition = 0, accessories = 0;
        std::array<unsigned char, 0x438> raw{};
        HeavyMpTrafficSample sample;
        bool okay = load(reader, table + i * 16, actor) && address(actor, raw.size()) &&
                    reader(actor, raw.data(), raw.size());
        if (!okay) {
            ++result.rejected;
            continue;
        }
        okay = value<Address>(raw.data(), 0) == engine + 0x2410ad8;
        physics = value<Address>(raw.data(), 0x210);
        definition = value<Address>(raw.data(), 0x410);
        okay = okay && load(reader, physics, vtable) && vtable == engine + 0x2416be8 &&
               load(reader, definition, vtable) && vtable == engine + 0x23513e0;
        okay = okay && load(reader, physics + 0x1c0, sample.lights) && sample.lights <= 0xffff;
        // Actor +0x28 is its navigation reference, offset from the rendered body.
        // The root model placement is the actual geometry transform written by
        // native physics virtual 0x30. A standalone replica has no driver offset.
        Address modelArray = 0, rootModel = 0;
        std::uint64_t modelCount = 0;
        heavympTrafficCaptureDetail::Placement placement{};
        okay = okay && load(reader, physics + 0x30, modelArray) && load(reader, physics + 0x38, modelCount) &&
               modelCount > 0 && modelCount <= 64 && load(reader, modelArray, rootModel) &&
               load(reader, rootModel + 0x30, placement);
        const auto originalId = value<std::uint32_t>(raw.data(), 0x420);
        okay = okay && originalId <= 0x3fffffff;
        sample.nativeId = originalId * 4;
        okay = okay && load(reader, physics + 0x1c4, sample.lookSeed) && std::isfinite(sample.lookSeed) &&
               sample.lookSeed >= 0 && sample.lookSeed <= 1;
        sample.vehicleType = value<std::uint32_t>(raw.data(), 0x70);
        if (okay)
            sample.hasPaintColor =
                paintColor(reader, engine, manager, definition, sample.lookSeed, sample.paintColor);
        sample.position = {placement.x + placement.sx * 512.0, placement.y,
                           placement.z + placement.sz * 512.0};
        sample.quaternion = {placement.w, placement.qx, placement.qy, placement.qz};
        double norm = 0;
        for (auto component : sample.quaternion) {
            okay = okay && std::isfinite(component);
            norm += component * component;
        }
        for (auto component : sample.position)
            okay = okay && std::isfinite(component) && std::abs(component) < 1e7;
        okay = okay && norm >= 0.9 && norm <= 1.1;
        std::uint64_t parts = 0, partCapacity = 0;
        okay = okay && load(reader, definition + 0x18, accessories) &&
               load(reader, definition + 0x20, parts) && load(reader, definition + 0x28, partCapacity) &&
               parts > 0 && parts <= 16 && parts <= partCapacity && partCapacity <= 64;
        for (std::size_t j = 0; okay && j < parts; ++j) {
            Address accessory = 0, path = 0;
            std::uint32_t length = 0, pathCapacity = 0;
            okay = load(reader, accessories + j * 8, accessory) && load(reader, accessory, vtable);
            HeavyMpAccessory part;
            if (vtable == engine + 0x2321490)
                part.kind = 1;
            else if (vtable == engine + 0x2328148)
                part.kind = 3;
            else if (vtable == engine + 0x23213c8)
                part.kind = 5;
            else if (vtable == engine + 0x2321dd8)
                part.kind = 4;
            else {
                okay = false;
                break;
            } // Unknown accessory needs explicit support first.
            okay = okay && load(reader, accessory + 0x18, path) && load(reader, accessory + 0x20, length) &&
                   load(reader, accessory + 0x24, pathCapacity) && length > 0 && length <= pathCapacity &&
                   pathCapacity <= 512 && length <= 256 && address(path, length);
            if (okay) {
                part.path.resize(length);
                okay = reader(path, part.path.data(), length) && heavympAsset(part.path);
            }
            if (okay && part.kind == 3)
                okay = load(reader, accessory + 0x40, part.flags) &&
                       reader(accessory + 0x44, part.colors.data(), 12);
            if (okay && part.kind == 4)
                okay = reader(accessory + 0x40, part.colors.data(), 72);
            if (okay)
                for (unsigned c = 0; c < (part.kind == 4 ? 18u : 3u); c++)
                    okay = okay && std::isfinite(part.colors[c]) && std::abs(part.colors[c]) <= 16;
            if (okay)
                sample.accessories.push_back(std::move(part));
        }
        std::uint32_t finalId = 0;
        Address finalDefinition = 0;
        okay = okay && load(reader, actor + 0x420, finalId) && finalId == originalId &&
               load(reader, actor + 0x410, finalDefinition) && finalDefinition == definition;
        if (okay) {
            result.vehicles.push_back(std::move(sample));
            Address trailer = value<Address>(raw.data(), 0x418);
            for (unsigned segment = 1; trailer && segment <= 3; segment++) {
                Address tp = 0, td = 0, tvt = 0, models = 0, root = 0, ap = 0;
                std::uint64_t mc = 0, pc = 0, cap = 0;
                HeavyMpTrafficSample ts;
                ts.nativeId = originalId * 4 + segment;
                ts.vehicleType = 255;
                bool good = load(reader, trailer, tvt) && tvt == engine + 0x2413818 &&
                            load(reader, trailer + 0xf8, tp) && load(reader, tp, tvt) &&
                            tvt == engine + 0x2417018 && load(reader, trailer + 0x2f0, td) &&
                            load(reader, td, tvt) && tvt == engine + 0x2351c98;
                Address physicalDef = 0;
                good = good && load(reader, tp + 0x228, physicalDef) && physicalDef == td &&
                       load(reader, tp + 0x30, models) && load(reader, tp + 0x38, mc) && mc > 0 && mc <= 64 &&
                       load(reader, models, root);
                heavympTrafficCaptureDetail::Placement pose{};
                good = good && load(reader, root + 0x30, pose) && load(reader, tp + 0x1c4, ts.lookSeed) &&
                       std::isfinite(ts.lookSeed) && ts.lookSeed >= 0 && ts.lookSeed <= 1 &&
                       load(reader, tp + 0x1c0, ts.lights) && ts.lights <= 0xffff;
                ts.position = {pose.x + pose.sx * 512.0, pose.y, pose.z + pose.sz * 512.0};
                for (auto p : ts.position)
                    good = good && std::isfinite(p) && std::abs(p) < 1e7;
                ts.quaternion = {pose.w, pose.qx, pose.qy, pose.qz};
                double qn = 0;
                for (auto q : ts.quaternion) {
                    good = good && std::isfinite(q);
                    qn += q * q;
                }
                good = good && qn >= .9 && qn <= 1.1;
                good = good && load(reader, td + 0x18, ap) && load(reader, td + 0x20, pc) &&
                       load(reader, td + 0x28, cap) && pc > 0 && pc <= 16 && pc <= cap && cap <= 64;
                for (size_t j = 0; good && j < pc; j++) {
                    Address a = 0, path = 0;
                    std::uint32_t length = 0, pathLimit = 0;
                    HeavyMpAccessory part;
                    good = load(reader, ap + j * 8, a) && load(reader, a, tvt);
                    part.kind = tvt == engine + 0x2321490   ? 1
                                : tvt == engine + 0x2328148 ? 3
                                : tvt == engine + 0x2321dd8 ? 4
                                                            : 0;
                    if (!part.kind) {
                        good = false;
                        break;
                    }
                    good = good && load(reader, a + 0x18, path) && load(reader, a + 0x20, length) &&
                           load(reader, a + 0x24, pathLimit) && length > 0 && length <= pathLimit &&
                           pathLimit <= 512 && length <= 256;
                    if (good) {
                        part.path.resize(length);
                        good = address(path, length) && reader(path, part.path.data(), length) &&
                               heavympAsset(part.path);
                    }
                    if (good && part.kind == 3)
                        good = load(reader, a + 0x40, part.flags) && reader(a + 0x44, part.colors.data(), 12);
                    if (good && part.kind == 4)
                        good = reader(a + 0x40, part.colors.data(), 72);
                    if (good)
                        for (unsigned c = 0; c < (part.kind == 4 ? 18u : 3u); c++)
                            good = good && std::isfinite(part.colors[c]) && std::abs(part.colors[c]) <= 16;
                    if (good)
                        ts.accessories.push_back(std::move(part));
                }
                Address finalPhysics = 0, finalTrailerDefinition = 0;
                good = good && load(reader, trailer + 0xf8, finalPhysics) && finalPhysics == tp &&
                       load(reader, trailer + 0x2f0, finalTrailerDefinition) && finalTrailerDefinition == td;
                if (good)
                    result.vehicles.push_back(std::move(ts));
                else {
                    ++result.rejected;
                    break;
                }
                Address next = 0;
                if (!load(reader, trailer + 0x2f8, next) || next == trailer)
                    break;
                trailer = next;
            }
        } else
            ++result.rejected;
    }
    Address finalManager = 0, finalTable = 0;
    std::uint64_t finalCount = 0;
    result.coherent = load(reader, engine + 0x3715000, finalManager) && finalManager == manager &&
                      load(reader, manager + 0xf8, finalTable) && finalTable == table &&
                      load(reader, manager + 0x100, finalCount) && finalCount == count;
    if (!result.coherent)
        result.vehicles.clear();
    return result;
}
