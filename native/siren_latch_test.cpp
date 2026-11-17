#include "heavymp_siren_latch.h"
#include <cstdio>
int main() {
    HeavyMpSirenLatch latch;
    const auto check = [](bool okay) {
        if (!okay) {
            std::fputs("Siren latch failed\n", stderr);
            return false;
        }
        return true;
    };
    if (!check(!latch.sample(1, true, false)))
        return 1;
    if (!check(latch.sample(1, true, true)))
        return 2;
    for (int frame = 0; frame < 120; ++frame)
        if (!check(latch.sample(1, true, true)))
            return 3;
    if (!check(latch.sample(1, true, false)))
        return 4; // release leaves it on
    if (!check(!latch.sample(1, true, true)))
        return 5; // second press stops
    if (!check(!latch.sample(1, true, true)))
        return 6;
    latch.sample(1, true, false);
    latch.sample(1, true, true);
    if (!check(!latch.sample(2, true, true)))
        return 7; // switch vehicle while held
    if (!check(!latch.sample(2, true, false)))
        return 8;
    if (!check(latch.sample(2, true, true)))
        return 9;
    if (!check(!latch.sample(2, false, false) && !latch.on))
        return 10;
    if (!check(!latch.sample(2, true, true)))
        return 11; // no phantom press on resume
    latch.sample(2, true, false);
    if (!check(latch.sample(2, true, true)))
        return 12;
    std::puts("PASS: press/release/hold/second press; vehicle switch; inactive reset.");
}
