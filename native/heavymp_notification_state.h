#pragma once
#include <cmath>
#include <cstdint>
inline bool heavyMpMoving(float speed, bool driving, std::uint64_t now, std::uint64_t at) {
    return driving && at && now >= at && now - at < 1500 && std::isfinite(speed) && std::abs(speed) > .01f;
}
