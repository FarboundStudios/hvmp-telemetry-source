#include "heavymp_work_budget.h"
#include <cassert>
#include <cstdio>
int main() {
    HeavyMpVisualBudget near, far;
    near.distance(20);
    far.distance(500);
    unsigned nearCalls = 0, farCalls = 0;
    for (unsigned at = 1; at <= 1000; ++at) {
        nearCalls += near.wheels(at);
        farCalls += far.wheels(at);
    }
    assert(nearCalls == 1000 && farCalls == 10);
    assert(!far.shadows() && near.shadows());
    far.distance(250);
    assert(far.band == 2);
    far.distance(219);
    assert(far.band == 1);
    far.distance(80);
    assert(far.band == 1);
    far.distance(69);
    assert(far.band == 0);
    far.distance(500);
    assert(far.models(1000, 1));
    assert(!far.models(1001, 1));
    assert(far.models(1002, 2));
    far.distance(-1);
    assert(far.band == 0); // unavailable camera keeps full quality
    HeavyMpLoadBudget load;
    load.reset(0);
    assert(load.take(HeavyMpLoadKind::Create, 0));
    assert(!load.take(HeavyMpLoadKind::Create, 0));
    assert(load.take(HeavyMpLoadKind::Poll, 1));
    assert(!load.take(HeavyMpLoadKind::Attach, 2));
    load.reset(5);
    assert(load.take(HeavyMpLoadKind::Attach, 5));
    assert(!load.take(HeavyMpLoadKind::Attach, 5));
    // Synthetic arrival storm: tractor + trailer creations/attachments cannot
    // all execute in the same frame, but the queue continues to make progress.
    unsigned created = 0, attached = 0;
    for (unsigned frame = 0; frame < 200; ++frame) {
        load.reset(frame * 16.0);
        for (unsigned i = 0; i < 160; ++i) {
            if (created < 160 && load.take(HeavyMpLoadKind::Create, frame * 16.0 + .1))
                ++created;
            if (attached < created && load.take(HeavyMpLoadKind::Attach, frame * 16.0 + .2))
                ++attached;
        }
        assert(load.creates <= 1 && load.attaches <= 1 && load.polls <= 4);
    }
    assert(created == 160 && attached == 160);
    HeavyMpDefinitionSlice definition;
    unsigned built = 0, slices = 0;
    while (definition.next < 256) {
        unsigned thisSlice = 0;
        const auto result = definition.advance(
            256, []() { return 0.; },
            [&](unsigned i) {
                assert(i == built);
                ++built;
                ++thisSlice;
                return true;
            });
        ++slices;
        assert(thisSlice <= 8);
        assert(result != HeavyMpBuildResult::Failed);
    }
    assert(slices == 32 && built == 256);
    definition = {};
    double clock = 0;
    assert(definition.advance(
               256, [&]() { return clock; },
               [&](unsigned) {
                   clock += 1.5;
                   return true;
               }) == HeavyMpBuildResult::Pending);
    assert(definition.next == 1);
    assert(definition.advance(
               256, []() { return 0.; }, [](unsigned) { return false; }) == HeavyMpBuildResult::Failed);
    puts("PASS: distance hysteresis, immediate lights, near-rate preservation, far work reduction, bounded "
         "progressive loads.");
}
