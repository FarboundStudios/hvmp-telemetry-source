#pragma once
#include <cmath>
#include <limits>
inline float heavyMpTrafficDensityMultiplier(unsigned base, unsigned target, bool source) {
    if (!source || !base || !target)
        return 0;
    // The native setter truncates base * multiplier. Round upward by one ULP to
    // avoid 31.999998 becoming 31; do not clamp the population at base or at 16.
    return std::nextafter(static_cast<float>(target) / base, std::numeric_limits<float>::infinity());
}
