#pragma once
#include "heavymp_traffic_capture.h"
#include <algorithm>
// Small two-sample interpolation buffer; no extrapolation across a stale feed.
class HeavyMpTrafficMotion {
    struct Pose {
        std::uint32_t id = 0;
        std::array<double, 3> position{};
        std::array<float, 4> quaternion{};
        double at = 0;
    };
    Pose previous{}, current{};
    bool initialized = false;

  public:
    void reset() {
        initialized = false;
    }
    void push(const HeavyMpTrafficSample& sample, double now) {
        if (initialized && now <= current.at)
            return;
        Pose next{sample.nativeId, sample.position, sample.quaternion, now};
        const double jump = initialized ? std::hypot(next.position[0] - current.position[0],
                                                     next.position[2] - current.position[2])
                                        : 0;
        if (!initialized || next.id != current.id || now - current.at > 1000 || jump > 30) {
            previous = current = next;
            initialized = true;
            return;
        }
        previous = current;
        current = next;
    }
    bool render(double now, HeavyMpTrafficSample& out) const {
        if (!initialized || now < current.at || now - current.at > 1500)
            return false;
        const double span = std::clamp(current.at - previous.at, 50.0, 500.0),
                     t = std::clamp((now - current.at) / span, 0.0, 1.0);
        out.nativeId = current.id;
        for (unsigned i = 0; i < 3; i++)
            out.position[i] = previous.position[i] + (current.position[i] - previous.position[i]) * t;
        double dot = 0, norm = 0;
        for (unsigned i = 0; i < 4; i++)
            dot += previous.quaternion[i] * current.quaternion[i];
        for (unsigned i = 0; i < 4; i++) {
            const double v = previous.quaternion[i] * (1 - t) + current.quaternion[i] * (dot < 0 ? -t : t);
            out.quaternion[i] = static_cast<float>(v);
            norm += v * v;
        }
        if (norm < .5 || !std::isfinite(norm))
            return false;
        for (auto& v : out.quaternion)
            v = static_cast<float>(v / std::sqrt(norm));
        return true;
    }
};
