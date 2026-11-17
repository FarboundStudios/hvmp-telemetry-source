#include "heavymp_traffic_lease.h"
#include <cassert>
#include <iostream>
int main() {
    assert(heavyMpTrafficSequenceAllowed(0, 10, 1000, 0, 900));
    assert(!heavyMpTrafficSequenceAllowed(100, 500, 1000, 0, 900));
    assert(!heavyMpTrafficSequenceAllowed(100, 1100, 1000, 900, 900));
    assert(heavyMpTrafficSequenceAllowed(100, 1101, 1000, 0, 900));
    assert(heavyMpTrafficSequenceAllowed(100, 500, 1000, 0, 900, false));
    assert(heavyMpTrafficSequenceAllowed(100, 101, 1000, 0, 0xffffffff));
    assert(!heavyMpTrafficSequenceAllowed(100, 101, 1000, 0xffffffff, 0));
    assert(heavyMpTrafficSequenceAllowed(100, 1601, 1500, 0, 500));
    std::cout
        << "Traffic leases: replay rejected, epoch restart, expiry recovery and sequence wrap passed.\n";
}
