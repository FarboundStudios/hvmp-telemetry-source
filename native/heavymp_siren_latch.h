#pragma once
#include <cstdint>
// Sample mapped input once per game frame; network readers only read latched on.
struct HeavyMpSirenLatch {
    std::uintptr_t actor = 0;
    bool on = false, pressed = false;
    void reset() {
        actor = 0;
        on = false;
        pressed = false;
    }
    bool sample(std::uintptr_t vehicle, bool eligible, bool down) {
        if (!eligible || !vehicle) {
            reset();
            return false;
        }
        if (actor != vehicle) {
            reset();
            actor = vehicle;
            pressed = down;
            return false;
        }
        if (down && !pressed)
            on = !on;
        pressed = down;
        return on;
    }
};
