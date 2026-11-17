#include "heavymp_snapshot_pool.h"
#include <cassert>
#include <cstdio>
struct State {
    int value = 0;
    std::shared_ptr<const State> next;
};
int main() {
    HeavyMpSnapshotPool<State> pool;
    auto previous = pool.acquire();
    previous->value = 42;
    auto active = pool.acquire();
    assert(active != previous && previous->value == 42);
    active->next = previous;
    const auto* first = previous.get();
    previous.reset();
    active.reset();
    auto reused = pool.acquire();
    assert(reused.get() != first || reused->value == 0);
    assert(!reused->next);
    reused.reset();
    const State* seen[3]{};
    for (unsigned i = 0; i < 10000; ++i) {
        auto state = pool.acquire();
        state->value = i;
        bool known = false;
        for (auto& address : seen) {
            if (address == state.get()) {
                known = true;
                break;
            }
            if (!address) {
                address = state.get();
                known = true;
                break;
            }
        }
        assert(known);
    }
    auto a = pool.acquire(), b = pool.acquire(), c = pool.acquire(), fallback = pool.acquire();
    assert(a != b && b != c && c != fallback && a != fallback);
    a->value = 99;
    assert(b->value == 0);
    puts(
        "PASS: immutable published snapshots, bounded reusable slots and safe retained-generation fallback.");
}
