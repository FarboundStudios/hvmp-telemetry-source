#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
// CPU visual work only. Placement, contact policy and freshness are never gated.
struct HeavyMpVisualBudget {
    unsigned band = 0;
    std::uint64_t wheelsAt = 0, modelsAt = 0;
    unsigned lights = ~0u;
    void distance(double metres) {
        if (!std::isfinite(metres) || metres < 0) {
            band = 0;
            return;
        }
        // Hysteresis prevents frequent quality changes at the thresholds.
        if (band == 0) {
            if (metres > 90)
                band = metres > 280 ? 2 : 1;
        } else if (band == 1) {
            if (metres < 70)
                band = 0;
            else if (metres > 280)
                band = 2;
        } else if (metres < 220)
            band = metres < 70 ? 0 : 1;
    }
    unsigned interval() const {
        return band == 0 ? 0 : band == 1 ? 33 : 100;
    }
    bool shadows() const {
        return band < 2;
    }
    static bool due(std::uint64_t now, std::uint64_t& at, unsigned interval) {
        if (at && now >= at && now - at < interval)
            return false;
        at = now;
        return true;
    }
    bool wheels(std::uint64_t now) {
        return due(now, wheelsAt, interval());
    }
    bool models(std::uint64_t now, unsigned currentLights) {
        const bool changed = lights != currentLights;
        lights = currentLights;
        if (changed) {
            modelsAt = now;
            return true;
        }
        return due(now, modelsAt, interval());
    }
};
enum class HeavyMpLoadKind { Create, Poll, Attach };
enum class HeavyMpBuildResult { Pending, Complete, Failed };
struct HeavyMpDefinitionSlice {
    unsigned next = 0;
    template <class Clock, class Build> HeavyMpBuildResult advance(unsigned count, Clock clock, Build build) {
        const double start = clock();
        unsigned steps = 0;
        while (next < count && steps < 8) {
            if (steps && clock() - start >= 1.0)
                break;
            if (!build(next))
                return HeavyMpBuildResult::Failed;
            ++next;
            ++steps;
        }
        return next == count ? HeavyMpBuildResult::Complete : HeavyMpBuildResult::Pending;
    }
};
struct HeavyMpLoadBudget {
    unsigned creates = 0, polls = 0, attaches = 0, work = 0;
    double start = 0;
    void reset(double now) {
        creates = polls = attaches = work = 0;
        start = now;
    }
    bool take(HeavyMpLoadKind kind, double now) {
        auto& counter = kind == HeavyMpLoadKind::Create ? creates
                        : kind == HeavyMpLoadKind::Poll ? polls
                                                        : attaches;
        const unsigned limit = kind == HeavyMpLoadKind::Poll ? 4 : 1;
        if (counter >= limit || (work && now - start >= 2.0))
            return false;
        ++counter;
        ++work;
        return true;
    }
};
