#include "heavymp_packet_inbox.h"
#include <cassert>
#include <thread>
#include <cstdio>
int main() {
    HeavyMpPacketInbox inbox;
    std::vector<unsigned char> state(16000), config(60000), policy(16), out;
    std::memcpy(state.data(), "HMV3", 4);
    std::memcpy(config.data(), "HMC2", 4);
    std::memcpy(policy.data(), "HMCL", 4);
    for (unsigned i = 0; i < 10000; ++i) {
        state[8] = static_cast<unsigned char>(i);
        assert(inbox.push(state.data(), state.size()));
    }
    assert(inbox.stats().packets == 1 && inbox.stats().replaced == 9999);
    assert(inbox.pop(out) && out[8] == static_cast<unsigned char>(9999));
    for (unsigned i = 0; i < 300; ++i)
        inbox.push(config.data(), config.size());
    assert(inbox.stats().bytes <= HeavyMpPacketInbox::maxBytes &&
           inbox.stats().packets <= HeavyMpPacketInbox::maxPackets && inbox.stats().dropped);
    assert(inbox.push(policy.data(), policy.size()));
    assert(inbox.pop(out) && !memcmp(out.data(), "HMCL", 4));
    std::thread producer([&]() {
        for (unsigned i = 0; i < 10000; ++i)
            inbox.push(state.data(), state.size());
    });
    for (unsigned i = 0; i < 10000; ++i)
        inbox.pop(out);
    producer.join();
    assert(inbox.stats().bytes <= HeavyMpPacketInbox::maxBytes);
    inbox.clear();
    assert(!inbox.pop(out) && !inbox.stats().packets && !inbox.stats().bytes);
    assert(!inbox.push(nullptr, 16) && !inbox.push(policy.data(), 4));
    puts("PASS: latest snapshot coalescing, bounded bytes/packets, immediate safety controls, concurrent "
         "producer/consumer and reset.");
}
