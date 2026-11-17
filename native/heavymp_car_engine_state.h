#pragma once
#include <cmath>
struct HeavyMpCarEngineState {
    enum Edge { none, start, stop };
    bool initialized = false, active = false, running = false;
    Edge sample(bool on, float rpm) {
        const bool nowActive = on || (std::isfinite(rpm) && rpm > 50.0f);
        if (!initialized) {
            initialized = true;
            active = nowActive;
            running = on;
            return none;
        }
        const Edge edge = running && !on ? stop : !active && nowActive ? start : none;
        active = nowActive;
        running = on;
        return edge;
    }
    void reset() {
        initialized = active = running = false;
    }
};
