#include "heavymp_traffic_lab_wire.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <fstream>
#include <iterator>
template <class T> void put(std::vector<unsigned char>& bytes, size_t offset, T v) {
    memcpy(bytes.data() + offset, &v, sizeof(v));
}
int main(int argc, char** argv) {
    const std::string path = "/def/vehicle/ai/golf/chassis.sii";
    std::vector<unsigned char> packet(88 + path.size());
    memcpy(packet.data(), "HIL1", 4);
    put(packet, 4, std::uint32_t(42));
    put(packet, 8, std::uint32_t(123));
    put(packet, 12, std::uint32_t(1));
    put(packet, 16, 100.0);
    put(packet, 24, 20.0);
    put(packet, 32, -500.0);
    put(packet, 40, 1.f);
    put(packet, 56, std::uint32_t(0x4200));
    put(packet, 64, std::uint32_t(1));
    put(packet, 72, static_cast<std::uint32_t>(path.size()));
    memcpy(packet.data() + 88, path.data(), path.size());
    HeavyMpTrafficSample sample;
    std::uint32_t sequence = 0;
    assert(heavympTrafficLabPacket(packet.data(), packet.size(), sample, sequence));
    assert(sample.nativeId == 123 && sequence == 42 && sample.position[2] == -500 &&
           sample.lights == 0x4200 && sample.accessories[0].path == path);
    for (size_t i = 0; i < packet.size(); i++) {
        sample.nativeId = 999;
        sequence = 777;
        assert(!heavympTrafficLabPacket(packet.data(), i, sample, sequence));
        assert(sample.nativeId == 999 && sequence == 777);
    }
    auto bad = packet;
    put(bad, 12, std::uint32_t(0xffffffff));
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    bad = packet;
    put(bad, 16, std::numeric_limits<double>::quiet_NaN());
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    bad = packet;
    put(bad, 40, 0.f);
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    bad = packet;
    put(bad, 60, std::uint32_t(1));
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    bad = packet;
    put(bad, 64, std::uint32_t(77));
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    bad = packet;
    bad[88 + 16] = '.';
    bad[88 + 17] = '.';
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    // HIL2 adds the exact native appearance seed and a trailer/body discriminator.
    const std::string trailerPath = "/def/vehicle/trailer/schw_cistern_food/chassis.sii",
                      paintPath = "/def/vehicle/trailer/schw_cistern_food/company_paint_job/mamont.sii";
    std::vector<unsigned char> v2(64 + 24 + trailerPath.size() + 84 + paintPath.size());
    memcpy(v2.data(), "HIL2", 4);
    put(v2, 4, std::uint32_t(17));
    put(v2, 8, std::uint32_t(85));
    put(v2, 12, std::uint32_t(0x80000002));
    put(v2, 40, 1.f);
    put(v2, 60, .625f);
    put(v2, 64, std::uint32_t(1));
    put(v2, 72, static_cast<std::uint32_t>(trailerPath.size()));
    memcpy(v2.data() + 88, trailerPath.data(), trailerPath.size());
    const size_t paint = 88 + trailerPath.size();
    put(v2, paint, std::uint32_t(4));
    put(v2, paint + 8, static_cast<std::uint32_t>(paintPath.size()));
    for (unsigned i = 0; i < 18; i++)
        put(v2, paint + 12 + i * 4, i * .01f);
    memcpy(v2.data() + paint + 84, paintPath.data(), paintPath.size());
    assert(heavympTrafficLabPacket(v2.data(), v2.size(), sample, sequence));
    assert(sample.vehicleType == 255 && sample.lookSeed == .625f &&
           sample.accessories[1].colors[17] == 17 * .01f);
    for (size_t i = 0; i < v2.size(); i++)
        assert(!heavympTrafficLabPacket(v2.data(), i, sample, sequence));
    bad = v2;
    put(bad, 60, std::numeric_limits<float>::quiet_NaN());
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    bad = v2;
    put(bad, paint + 12 + 17 * 4, std::numeric_limits<float>::infinity());
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    bad = v2;
    put(bad, 12, std::uint32_t(2));
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample,
                                    sequence)); // A trailer cannot use the car loader.
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        assert(file);
        const std::vector<unsigned char> fromNode{std::istreambuf_iterator<char>(file),
                                                  std::istreambuf_iterator<char>()};
        assert(heavympTrafficLabPacket(fromNode.data(), fromNode.size(), sample, sequence));
        assert(sequence == 17 && sample.vehicleType == 255 &&
               std::abs(sample.lookSeed - .2569880485534668f) < 1e-7f);
        assert(sample.accessories.size() == 5 &&
               sample.accessories[0].path == "/def/vehicle/trailer/tirsan_scs/ch_3.sii" &&
               sample.accessories[2].flags == 65538 && sample.accessories[3].flags == 131076 &&
               sample.accessories[4].kind == 4);
        std::cout << "PASS: Node HIL2 encoder -> native decoder with captured SCS trailer appearance.\n";
    }
    // The colour is resolved by the source, not re-picked from a local palette.
    std::vector<unsigned char> colored = packet;
    colored.insert(colored.begin() + 64, 12, 0);
    memcpy(colored.data(), "HIL3", 4);
    put(colored, 12, std::uint32_t(0x40000001));
    put(colored, 64, .1f);
    put(colored, 68, .0527f);
    put(colored, 72, .2177f);
    assert(heavympTrafficLabPacket(colored.data(), colored.size(), sample, sequence) &&
           sample.hasPaintColor && sample.paintColor[1] == .0527f);
    bad = colored;
    put(bad, 68, std::numeric_limits<float>::quiet_NaN());
    assert(!heavympTrafficLabPacket(bad.data(), bad.size(), sample, sequence));
    for (size_t i = 0; i < colored.size(); i++)
        assert(!heavympTrafficLabPacket(colored.data(), i, sample, sequence));
    std::cout << "Traffic wire: pose/model/linear RGB/lights, every truncation, atomic rejection, bounds, "
                 "bad floats/quaternion/path/kind passed.\n";
}
