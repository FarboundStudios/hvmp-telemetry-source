#include "heavymp_traffic_motion.h"
#include <cassert>
#include <iostream>
int main() {
    HeavyMpTrafficMotion motion;
    HeavyMpTrafficSample a, b, out;
    a.nativeId = b.nativeId = 7;
    a.position = {0, 0, 0};
    b.position = {10, 0, 0};
    a.quaternion = {1, 0, 0, 0};
    b.quaternion = {-1, 0, 0, 0};
    assert(!motion.render(0, out));
    motion.push(a, 0);
    motion.push(b, 100);
    assert(motion.render(150, out) && out.position[0] == 5 && std::abs(out.quaternion[0]) == 1);
    assert(motion.render(200, out) && out.position[0] == 10);
    assert(!motion.render(1601, out));
    b.position[0] = 1000;
    motion.push(b, 250);
    assert(motion.render(250, out) && out.position[0] == 1000);
    b.nativeId = 8;
    b.position[0] = 30;
    motion.push(b, 300);
    assert(motion.render(300, out) && out.position[0] == 30);
    b.position[0] = 40;
    motion.push(b, 290);
    assert(motion.render(300, out) && out.position[0] == 30);
    motion.reset();
    assert(!motion.render(400, out));
    motion.reset();
    a.position = {0, 0, 0};
    b.nativeId = a.nativeId;
    b.position = {10, 0, 0};
    motion.push(a, 0);
    motion.push(b, 500);
    assert(motion.render(750, out) && out.position[0] == 5);
    std::cout << "Traffic motion: interpolation, quaternion hemisphere, stale feed, jump/identity reset and "
                 "old samples passed.\n";
}
