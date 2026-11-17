#include "heavymp_collision_policy.h"
#include <cassert>
#include <cstdio>
int main() {
    HeavyMpCollisionAudit audit;
    assert(audit.due(false, 1000));
    assert(!audit.due(false, 1099));
    assert(audit.due(false, 1100));
    assert(audit.due(true, 1101));
    assert(audit.due(false, 1102));
    assert(audit.due(false, 500)); // clock reset never prevents a safety audit
    HeavyMpPlacementContinuity placement;
    assert(!placement.update(0, 0, 0, 20, 1000));
    assert(!placement.update(2, 0, 0, 20, 1100));
    assert(placement.update(1000, 0, 0, 20, 1200));
    assert(!placement.update(1001, 0, 0, 20, 1250));
    assert(placement.update(1001, 20, 0, 0, 1300));
    assert(placement.update(1001, 20, 0, 0, 4000));
    // Normal fast motorway travel at 110 m/s with a delayed sample is plausible.
    assert(!placement.update(1101, 20, 0, 110, 5000));
    // Replay the real ETS2 1.61.1.9s matrix read on 2026-10-19. The old
    // category was rejected even with the native ghost flag cleared.
    const std::uint32_t rows[] = {4288674110u, 4288674102u, 4288608766u};
    for (unsigned local = 6; local <= 7; ++local) {
        assert(!(rows[local - 6] & (1u << 6)) && !(rows[local - 6] & (1u << 7)));
        assert(rows[local - 6] & (1u << heavyMpRemoteContactCategory(6)));
        assert(rows[local - 6] & (1u << heavyMpRemoteContactCategory(7)));
    }
    assert(heavyMpRemoteContactCategory(8) == 8 && heavyMpRemoteContactCategory(3) == 3);
    unsigned char packet[16]{};
    std::memcpy(packet, "HMCL", 4);
    bool enabled = true;
    assert(heavyMpCollisionPacket(packet, 16, &enabled) && !enabled);
    packet[8] = 1;
    assert(heavyMpCollisionPacket(packet, 16, &enabled) && enabled);
    packet[8] = 2;
    assert(!heavyMpCollisionPacket(packet, 16, &enabled));
    packet[8] = 1;
    packet[12] = 1;
    assert(!heavyMpCollisionPacket(packet, 16, &enabled));
    packet[12] = 0;
    assert(!heavyMpCollisionPacket(packet, 15, &enabled));
    assert(!heavyMpCollisionPacket(nullptr, 16, &enabled));
    assert(!heavyMpCollisionLease(100, 0) && heavyMpCollisionLease(100, 1600));
    assert(!heavyMpCollisionLease(1600, 1600) && !heavyMpCollisionLease(1700, 1600));
    // Regression: individual chassis shapes alone did not restore cabin contacts.
    // The bridge must call the tractor's full native ghost setter on first apply,
    // even if its native ghost flag originally looked disabled.
    HeavyMpCollisionSwitch transition;
    bool chassis = false, cabin = false;
    unsigned calls = 0;
    const auto nativeGhost = [&](unsigned char ghost) {
        ++calls;
        chassis = cabin = ghost == 0;
    };
    transition.apply(true, nativeGhost);
    assert(chassis && cabin && calls == 1);
    transition.apply(true, nativeGhost);
    assert(calls == 1);
    transition.apply(false, nativeGhost);
    assert(!chassis && !cabin && calls == 2);
    transition.apply(true, nativeGhost);
    assert(chassis && cabin && calls == 3);
    transition = {};
    chassis = cabin = false;
    transition.apply(true, nativeGhost);
    assert(chassis && cabin && calls == 4);
    puts("PASS: collision packet validation and expired/disconnected ghost mode.");
    puts("PASS: tractor ghost transition restores chassis and cabin, initial state and respawn included.");
}
