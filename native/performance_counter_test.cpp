#include "heavymp_performance.h"
#include <cassert>
#include <thread>
#include <vector>
#include <cstdio>
int main() {
    HeavyMpPerfCounter counter;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i)
        threads.emplace_back([&]() {
            for (unsigned j = 1; j <= 1000; ++j)
                counter.add(j);
        });
    for (auto& thread : threads)
        thread.join();
    const auto sample = counter.take();
    assert(sample.calls == 8000 && sample.ticks == 8 * 500500 && sample.peak == 1000);
    const auto empty = counter.take();
    assert(!empty.calls && !empty.ticks && !empty.peak);
    const auto start = heavyMpPerfClock();
    Sleep(10);
    const auto elapsed = heavyMpPerfMs(heavyMpPerfClock() - start);
    assert(elapsed >= 1 && elapsed < 1000);
    HeavyMpFrameCadence cadence;
    cadence.record(1, true);
    for (unsigned i = 1; i <= 99; ++i)
        cadence.record(1 + i * 16., true);
    cadence.record(2000, true);
    auto report = cadence.take();
    assert(report.count == 100 && report.p50 == 16 && report.p95 == 16 && report.maximum == 415 &&
           report.overBudget == 1);
    cadence.record(5000, false);
    cadence.record(10000, true);
    assert(!cadence.take().count);
    puts("PASS: concurrent CPU timing totals/maximum, window reset, QPC milliseconds.");
}
