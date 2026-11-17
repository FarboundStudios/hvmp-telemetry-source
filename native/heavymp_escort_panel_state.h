#pragma once
#include <algorithm>
#include <cstdint>
constexpr unsigned heavyMpPanelLightBit = 1u << 30;
struct HeavyMpPanelRig {
    std::uintptr_t model = 0;
    int index = -1;
    float phase = 0;
    std::uint64_t at = 0, retryAt = 0;
    bool fault = false;
    void advance(bool open, std::uint64_t now) {
        const float dt = at ? std::clamp(static_cast<float>(now - at) * .001f, 0.f, .1f) : 0.f;
        at = now;
        phase = std::clamp(phase + (open ? dt : -dt), 0.f, 1.f);
    }
};
