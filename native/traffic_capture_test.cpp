#include "heavymp_traffic_capture.h"
#include <cassert>
#include <map>
#include <iostream>
#include <limits>
#include "heavymp_traffic_population.h"
struct Memory {
    std::map<std::uint64_t, unsigned char> bytes;
    std::size_t reads = 0;
    template <class T> void put(std::uint64_t at, T value) {
        const auto* p = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes[at + i] = p[i];
    }
    void text(std::uint64_t at, const std::string& value) {
        for (std::size_t i = 0; i < value.size(); ++i)
            bytes[at + i] = static_cast<unsigned char>(value[i]);
    }
    bool read(std::uint64_t at, void* output, std::size_t count) {
        ++reads;
        assert(count <= 65536);
        auto* out = static_cast<unsigned char*>(output);
        for (std::size_t i = 0; i < count; ++i) {
            auto found = bytes.find(at + i);
            if (found == bytes.end())
                return false;
            out[i] = found->second;
        }
        return true;
    }
};
int main() {
    constexpr std::uint64_t base = 0x140000000, manager = 0x100000, table = 0x200000, actor = 0x300000,
                            physics = 0x400000, definition = 0x500000, parts = 0x600000, part = 0x700000,
                            path = 0x800000;
    Memory memory;
    memory.put(base + 0x3715000, manager);
    memory.put(manager, base + 0x240b190);
    memory.put(manager + 0xf8, table);
    memory.put(manager + 0x100, std::uint64_t(1));
    memory.put(manager + 0x108, std::uint64_t(50));
    memory.put(table, actor);
    for (unsigned i = 0; i < 0x438; ++i)
        memory.put(actor + i, static_cast<unsigned char>(0));
    memory.put(actor, base + 0x2410ad8);
    memory.put(actor + 0x210, physics);
    memory.put(physics, base + 0x2416be8);
    memory.put(actor + 0x410, definition);
    memory.put(definition, base + 0x23513e0);
    memory.put(physics + 0x1c0, std::uint32_t(0));
    memory.put(physics + 0x1c4, .75f);
    memory.put(actor + 0x420, std::uint32_t(21));
    memory.put(definition + 0x1f8, base + 0x2252190);
    memory.put(definition + 0x200, std::uint64_t(0x1300000));
    memory.put(definition + 0x208, std::uint64_t(3));
    memory.put(definition + 0x210, std::uint64_t(3));
    for (unsigned i = 0; i < 9; i++)
        memory.put(0x1300000 + i * 4, i * .1f);
    memory.put(actor + 0x28, heavympTrafficCaptureDetail::Placement{10, 20, 30, -2, 3, 1, 0, 0, 0});
    memory.put(physics + 0x30, std::uint64_t(0x900000));
    memory.put(physics + 0x38, std::uint64_t(1));
    memory.put(0x900000, std::uint64_t(0xa00000));
    memory.put(0xa00030, heavympTrafficCaptureDetail::Placement{10, 20, 30, -2, 3, 1, 0, 0, 0});
    memory.put(definition + 0x18, parts);
    memory.put(definition + 0x20, std::uint64_t(1));
    memory.put(definition + 0x28, std::uint64_t(1));
    memory.put(parts, part);
    memory.put(part, base + 0x2321490);
    memory.put(part + 0x18, path);
    const std::string asset = "/def/vehicle/ai/bgeed/chassis.sii";
    memory.put(part + 0x20, static_cast<std::uint32_t>(asset.size()));
    memory.put(part + 0x24, std::uint32_t(128));
    memory.text(path, asset);
    auto capture = [&](bool verified = true) {
        return heavympCaptureTraffic(
            base, verified, [&](auto at, auto output, auto count) { return memory.read(at, output, count); });
    };
    auto result = capture();
    assert(result.coherent && result.vehicles.size() == 1 && result.rejected == 0);
    assert(result.vehicles[0].nativeId == 84 && result.vehicles[0].position[0] == -1014 &&
           result.vehicles[0].position[2] == 1566 && result.vehicles[0].accessories[0].path == asset);
    assert(result.vehicles[0].lookSeed == .75f && result.vehicles[0].hasPaintColor &&
           result.vehicles[0].paintColor[0] == 6 * .1f);
    memory.put(definition + 0x208, std::uint64_t(0));
    memory.put(manager + 0x20, std::uint64_t(0x1400000));
    memory.put(0x1400010, base + 0x2252190);
    memory.put(0x1400018, std::uint64_t(0x1300000));
    memory.put(0x1400020, std::uint64_t(3));
    memory.put(0x1400028, std::uint64_t(3));
    assert(capture().vehicles[0].hasPaintColor && capture().vehicles[0].paintColor[0] == 6 * .1f);
    memory.put(definition + 0x208, std::uint64_t(3));
    auto reads = memory.reads;
    assert(!capture(false).coherent && memory.reads == reads);
    memory.put(manager + 0x100, std::uint64_t(5000));
    assert(!capture().coherent);
    memory.put(manager + 0x100, std::uint64_t(1));
    memory.put(0xa00030, std::numeric_limits<float>::quiet_NaN());
    assert(capture().vehicles.empty());
    memory.put(0xa00030, 10.f);
    memory.put(part + 0x20, std::uint32_t(100000));
    assert(capture().vehicles.empty());
    memory.put(part + 0x20, static_cast<std::uint32_t>(asset.size()));
    memory.put(physics, base + 1);
    assert(capture().vehicles.empty());
    memory.put(physics, base + 0x2416be8);
    bool changed = false;
    result = heavympCaptureTraffic(base, true, [&](auto at, auto output, auto count) {
        if (at == actor + 0x420) {
            memory.put(actor + 0x420, std::uint32_t(22));
            changed = true;
        }
        return memory.read(at, output, count);
    });
    assert(changed && result.coherent && result.vehicles.empty());
    memory.put(actor + 0x420, std::uint32_t(21));
    unsigned tableReads = 0;
    result = heavympCaptureTraffic(base, true, [&](auto at, auto output, auto count) {
        if (at == manager + 0xf8 && ++tableReads == 2)
            memory.put(manager + 0xf8, table + 16);
        return memory.read(at, output, count);
    });
    assert(!result.coherent && result.vehicles.empty());
    // A native trailer is a separate body with its own look, wheel flags and paint.
    memory.put(manager + 0xf8, table);
    constexpr std::uint64_t trailer = 0xb00000, tp = 0xc00000, td = 0xd00000, tparts = 0xe00000,
                            modelList = 0xf00000, tmodel = 0x1000000;
    memory.put(actor + 0x418, trailer);
    memory.put(trailer, base + 0x2413818);
    memory.put(trailer + 0xf8, tp);
    memory.put(trailer + 0x2f0, td);
    memory.put(trailer + 0x2f8, std::uint64_t(0));
    memory.put(tp, base + 0x2417018);
    memory.put(tp + 0x228, td);
    memory.put(td, base + 0x2351c98);
    memory.put(tp + 0x30, modelList);
    memory.put(tp + 0x38, std::uint64_t(1));
    memory.put(modelList, tmodel);
    memory.put(tmodel + 0x30, heavympTrafficCaptureDetail::Placement{5, 20, 10, -2, 3, 1, 0, 0, 0});
    memory.put(tp + 0x1c0, std::uint32_t(4));
    memory.put(tp + 0x1c4, .25f);
    memory.put(td + 0x18, tparts);
    memory.put(td + 0x20, std::uint64_t(3));
    memory.put(td + 0x28, std::uint64_t(3));
    const std::array<std::string, 3> paths = {
        "/def/vehicle/trailer/schw_cistern_food/chassis.sii", "/def/vehicle/ai_wheel/rear_22in.sii",
        "/def/vehicle/trailer/schw_cistern_food/company_paint_job/mamont.sii"};
    for (unsigned i = 0; i < 3; i++) {
        const auto a = std::uint64_t(0x1100000 + i * 0x1000), p = std::uint64_t(0x1200000 + i * 0x1000);
        memory.put(tparts + i * 8, a);
        memory.put(a, base + (i == 0 ? 0x2321490 : i == 1 ? 0x2328148 : 0x2321dd8));
        memory.put(a + 0x18, p);
        memory.put(a + 0x20, static_cast<std::uint32_t>(paths[i].size()));
        memory.put(a + 0x24, std::uint32_t(256));
        memory.text(p, paths[i]);
        if (i == 1) {
            memory.put(a + 0x40, std::uint32_t(65536));
            for (unsigned c = 0; c < 3; c++)
                memory.put(a + 0x44 + c * 4, .5f);
        }
        if (i == 2)
            for (unsigned c = 0; c < 18; c++)
                memory.put(a + 0x40 + c * 4, c * .01f);
    }
    result = capture();
    assert(result.coherent && result.nativeCount == 1 && result.vehicles.size() == 2 && result.rejected == 0);
    const auto& t = result.vehicles[1];
    assert(t.nativeId == 85 && t.vehicleType == 255 && t.lookSeed == .25f && t.position[0] == -1019 &&
           t.lights == 4);
    assert(t.accessories[1].flags == 65536 && t.accessories[1].colors[2] == .5f &&
           t.accessories[2].colors[17] == 17 * .01f);
    memory.put(tmodel + 0x30, std::numeric_limits<float>::infinity());
    assert(capture().vehicles.size() == 1);
    memory.put(tmodel + 0x30, 5.f);
    memory.put(trailer + 0x2f8, trailer);
    assert(capture().vehicles.size() == 2); // Self links never loop.
    memory.put(trailer + 0x2f8, std::uint64_t(0));
    result = heavympCaptureTraffic(base, true, [&](auto at, auto output, auto count) {
        if (at == trailer + 0x2f0 && count == 8)
            memory.put(trailer + 0xf8, tp + 1);
        return memory.read(at, output, count);
    });
    assert(result.vehicles.size() == 1); // A concurrently recycled trailer is omitted.
    memory.put(trailer + 0xf8, tp);
    memory.put(actor + 0x418, std::uint64_t(0));
    memory.put(manager + 0x100, std::uint64_t(40));
    memory.put(manager + 0x108, std::uint64_t(50));
    for (unsigned i = 1; i < 40; i++) {
        const auto a = actor + i * 0x1000;
        for (unsigned j = 0; j < 0x438; j++)
            memory.put(a + j, memory.bytes.at(actor + j));
        memory.put(a + 0x420, std::uint32_t(21 + i));
        memory.put(table + i * 16, a);
    }
    result = heavympCaptureTraffic(
        base, true, [&](auto at, auto output, auto count) { return memory.read(at, output, count); }, 64);
    assert(result.coherent && result.vehicles.size() == 40);
    assert(capture().vehicles.size() == 32); // The diagnostic default is not the runtime population.
    for (unsigned target : {1u, 8u, 31u, 32u, 64u, 128u, 256u, 1000u})
        assert(static_cast<unsigned>(50.f * heavyMpTrafficDensityMultiplier(50, target, true)) == target);
    assert(heavyMpTrafficDensityMultiplier(50, 64, false) == 0);
    std::cout << "Traffic capture: placement, model paths, version gate, bounds, invalid floats, unavailable "
                 "physics and concurrent recycling passed.\n";
}
