#pragma once
#include <cstdint>
// A new network worker can restart sequence numbers without restarting ETS2.
// Reject replay while the lease is live, permit recovery once it has expired.
inline bool heavyMpTrafficSequenceAllowed(std::uint64_t lastAt, std::uint64_t now, std::uint64_t lease,
                                          std::uint32_t next, std::uint32_t previous, bool sameEpoch = true) {
    return !sameEpoch || !lastAt || now < lastAt || now - lastAt > lease ||
           static_cast<std::int32_t>(next - previous) > 0;
}
