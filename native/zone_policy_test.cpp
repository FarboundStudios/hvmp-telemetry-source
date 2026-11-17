#include "heavymp_zone_policy.h"
#include <cassert>
#include <cstdio>
int main() {
    unsigned char data[16 + 32 * 16]{};
    memcpy(data, "HNC1", 4);
    HeavyMpZonePolicy p;
    std::array<unsigned char, 16> owner{};
    owner[0] = 42;
    data[8] = 7;
    assert(heavyMpZonePacket(data, 16, &p));
    assert(p.indicator(10, 20) && p.ghost(owner, 10, 20));
    assert(!p.indicator(20, 20) && p.ghost(owner, 20, 20));
    data[8] = 5;
    data[12] = 1;
    memcpy(data + 16, owner.data(), 16);
    assert(heavyMpZonePacket(data, 32, &p));
    assert(!p.indicator(10, 20) && p.ghost(owner, 10, 20));
    owner[0] = 43;
    assert(!p.ghost(owner, 10, 20));
    assert(p.ghost(owner, 21, 20));
    data[8] = 1;
    data[12] = 0;
    assert(heavyMpZonePacket(data, 16, &p) && p.ghost(owner, 10, 20));
    data[8] = 0;
    assert(heavyMpZonePacket(data, 16, &p) && !p.ghost(owner, 10, 20));
    data[8] = 2;
    assert(!heavyMpZonePacket(data, 16, &p));
    data[8] = 5;
    data[12] = 33;
    assert(!heavyMpZonePacket(data, sizeof(data), &p));
    data[12] = 1;
    assert(!heavyMpZonePacket(data, 16, &p));
    assert(!heavyMpZonePacket(nullptr, 16, &p));
    assert(!heavyMpZonePacket(data, 32, nullptr));
    puts("PASS: HNC1 packet bounds, per-owner policy, expired leases and independent red indicator.");
}
