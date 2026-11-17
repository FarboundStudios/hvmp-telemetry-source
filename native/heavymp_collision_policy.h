#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
// Re-scan owned shapes on transitions immediately, then at most ten times a
// second. Never cache engine body pointers across frames or asset reloads.
struct HeavyMpCollisionAudit {
    int mode = -1;
    std::uint64_t at = 0;
    bool due(bool collide, std::uint64_t now) {
        if (mode == static_cast<int>(collide) && now >= at && now - at < 100)
            return false;
        mode = collide ? 1 : 0;
        at = now;
        return true;
    }
};
// Placement discontinuities must disarm contacts BEFORE native relocation.
struct HeavyMpPlacementContinuity {
    bool valid = false;
    double x = 0, y = 0, z = 0;
    std::uint64_t at = 0;
    bool update(double nx, double ny, double nz, double speed, std::uint64_t now) {
        const double elapsed = now >= at ? static_cast<double>(now - at) / 1000 : 0;
        const double limit = std::max(50.0, std::min(110.0, std::abs(speed)) * elapsed * 2 + 10);
        const bool jump = valid && (now < at || elapsed > 1.5 || std::hypot(nx - x, nz - z) > limit ||
                                    std::abs(ny - y) > 12);
        valid = true;
        x = nx;
        y = ny;
        z = nz;
        at = now;
        return jump;
    }
};
// No native pointers: this lease is independently testable and expires if the
// launcher stops sending updates. Decoding happens on the UDP receiver thread.
inline bool heavyMpCollisionPacket(const unsigned char* data, std::size_t size, bool* enabled) {
    if (!data || !enabled || size != 16 || std::memcmp(data, "HMCL", 4))
        return false;
    std::uint32_t flags = 0, reserved = 0;
    std::memcpy(&flags, data + 8, 4);
    std::memcpy(&reserved, data + 12, 4);
    if (flags > 1 || reserved)
        return false;
    *enabled = flags == 1;
    return true;
}
inline bool heavyMpCollisionLease(std::uint64_t now, std::uint64_t until) {
    return until != 0 && now < until;
}
// ETS2's local tractor/cabin categories (6/7) deliberately reject each other.
// Remote kinematic contacts use category 8, already used by remote trailers.
// Do not modify the scene matrix: it also prevents local cab/chassis self hits.
inline unsigned heavyMpRemoteContactCategory(unsigned category) {
    return category == 6 || category == 7 ? 8 : category;
}
struct HeavyMpCollisionSwitch {
    int applied = -1;
    template <class SetGhost> void apply(bool collide, SetGhost setGhost) {
        if (applied == static_cast<int>(collide))
            return;
        // ETS2's native API takes "ghost", the inverse of server collisions.
        // First application is mandatory even when the engine flag defaults to zero:
        // our asynchronous creation path has already disabled individual shapes.
        setGhost(static_cast<unsigned char>(!collide));
        applied = collide ? 1 : 0;
    }
};
