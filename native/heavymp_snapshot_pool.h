#pragma once
#include <array>
#include <memory>
// A published snapshot is immutable until all native readers release it.
// Reuse only a slot owned by this pool alone; never mutate a live draw snapshot.
template <class T, unsigned Count = 3> struct HeavyMpSnapshotPool {
    std::array<std::shared_ptr<T>, Count> slots{};
    std::shared_ptr<T> acquire() {
        for (auto& slot : slots)
            if (!slot || slot.use_count() == 1) {
                if (!slot)
                    slot = std::make_shared<T>();
                else
                    *slot = T{};
                return slot;
            }
        // Keep correctness if a future consumer retains more than three generations.
        return std::make_shared<T>();
    }
};
