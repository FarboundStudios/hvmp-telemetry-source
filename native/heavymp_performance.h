#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <array>
#include <algorithm>
// CPU wall time only. These measurements are not GPU presentation FPS.
inline std::uint64_t heavyMpPerfClock() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
inline double heavyMpPerfMs(std::uint64_t ticks) {
    static const double frequency = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return ticks * 1000.0 / frequency;
}
struct HeavyMpPerfSample {
    std::uint64_t calls = 0, ticks = 0, peak = 0;
};
struct HeavyMpPerfCounter {
    std::atomic<std::uint64_t> calls{0}, ticks{0}, peak{0};
    void add(std::uint64_t elapsed) {
        calls.fetch_add(1, std::memory_order_relaxed);
        ticks.fetch_add(elapsed, std::memory_order_relaxed);
        auto previous = peak.load(std::memory_order_relaxed);
        while (elapsed > previous &&
               !peak.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed)) {
        }
    }
    HeavyMpPerfSample take() {
        return {calls.exchange(0), ticks.exchange(0), peak.exchange(0)};
    }
};
// SDK callback cadence, distinct from GPU Present/display measurements.
struct HeavyMpFrameCadence {
    std::array<double, 2048> samples{};
    unsigned count = 0, index = 0;
    double previous = 0;
    void record(double now, bool active) {
        if (!active) {
            previous = 0;
            return;
        }
        if (previous && now > previous) {
            samples[index] = now - previous;
            index = (index + 1) % samples.size();
            count = std::min<unsigned>(count + 1, static_cast<unsigned>(samples.size()));
        }
        previous = now;
    }
    struct Report {
        unsigned count = 0, overBudget = 0;
        double p50 = 0, p95 = 0, p99 = 0, maximum = 0;
    };
    Report take() {
        Report result;
        result.count = count;
        if (!count)
            return result;
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.begin() + count);
        result.p50 = sorted[(count - 1) * 50 / 100];
        result.p95 = sorted[(count - 1) * 95 / 100];
        result.p99 = sorted[(count - 1) * 99 / 100];
        result.maximum = sorted[count - 1];
        for (unsigned i = 0; i < count; ++i)
            result.overBudget += sorted[i] > 1000.0 / 60;
        count = index = 0;
        return result;
    }
};
