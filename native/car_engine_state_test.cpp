#include "heavymp_car_engine_state.h"
#include <cassert>
#include <cstdio>
int main() {
    using S = HeavyMpCarEngineState;
    S s;
    assert(s.sample(true, 800) == S::none); // joining running car never starts it again
    assert(s.sample(false, 600) == S::stop);
    assert(s.sample(false, 0) == S::none);
    assert(s.sample(false, 160) == S::start); // cranking before engine-enabled
    assert(s.sample(false, 240) == S::none);
    assert(s.sample(true, 800) == S::none);
    for (int i = 0; i < 300; ++i)
        assert(s.sample(true, 800) == S::none);
    assert(s.sample(false, 600) == S::stop);
    assert(s.sample(false, 0) == S::none);
    assert(s.sample(true, 800) == S::start); // SDK may go directly to running
    s.reset();
    assert(s.sample(true, 800) == S::none); // resume/pause snapshot
    s.reset();
    assert(s.sample(false, 0) == S::none);
    assert(s.sample(false, 200) == S::start);
    assert(s.sample(false, 0) == S::none);
    assert(s.sample(false, 200) == S::start); // retry after failed cranking
    std::puts("PASS: idle persistence, cranking/start/stop edges, retry, join and resume snapshots.");
}
