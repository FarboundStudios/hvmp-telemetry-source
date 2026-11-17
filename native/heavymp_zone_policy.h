#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
struct HeavyMpZonePolicy {
    unsigned flags = 0, count = 0;
    std::array<std::array<unsigned char, 16>, 32> blocked{};
    bool ghost(const std::array<unsigned char, 16>& owner, std::uint64_t now, std::uint64_t until) const {
        if (!(flags & 1))
            return false;
        if (!(flags & 4) || now >= until || (flags & 2))
            return true;
        for (unsigned i = 0; i < count; ++i)
            if (blocked[i] == owner)
                return true;
        return false;
    }
    bool indicator(std::uint64_t now, std::uint64_t until) const {
        return (flags & 7) == 7 && now < until;
    }
};
inline bool heavyMpZonePacket(const unsigned char* p, std::size_t size, HeavyMpZonePolicy* out) {
    if (!p || !out || size < 16 || std::memcmp(p, "HNC1", 4))
        return false;
    unsigned flags = 0, count = 0;
    std::memcpy(&flags, p + 8, 4);
    std::memcpy(&count, p + 12, 4);
    if (flags > 7 || count > 32 || size != 16 + count * 16 || (!(flags & 1) && (flags || count)) ||
        (!(flags & 4) && (count || (flags & 2))))
        return false;
    HeavyMpZonePolicy result;
    result.flags = flags;
    result.count = count;
    for (unsigned i = 0; i < count; ++i)
        std::memcpy(result.blocked[i].data(), p + 16 + i * 16, 16);
    *out = result;
    return true;
}
