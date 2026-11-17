#pragma once
#include <vector>
#include <deque>
#include <mutex>
#include <cstring>
#include <cstdint>
// The receiver owns no game objects. Only the SDK frame consumes this queue.
class HeavyMpPacketInbox {
    std::mutex mutex;
    std::deque<std::vector<unsigned char>> queue;
    std::size_t bytes = 0;
    std::uint64_t replaced = 0, dropped = 0;
    static bool type(const unsigned char* p, const char* name) {
        return std::memcmp(p, name, 4) == 0;
    }
    static bool latest(const unsigned char* p) {
        return type(p, "HIA1") || type(p, "HIV1") || type(p, "HMV1") || type(p, "HMV2") || type(p, "HMV3") ||
               type(p, "HMT1") || type(p, "HMT2") || type(p, "HMT3") || type(p, "HMCL") || type(p, "HNC1") ||
               type(p, "HME1") || type(p, "HML1") || type(p, "HMW1") || type(p, "HMS1") || type(p, "HMP1") ||
               type(p, "HMX1") || type(p, "HMX2") || type(p, "HMX3");
    }
    static bool urgent(const unsigned char* p) {
        return type(p, "HIA1") || type(p, "HMCL") || type(p, "HNC1") || type(p, "HME1") || type(p, "HML1") ||
               type(p, "HMD1") || type(p, "HMR1");
    }

  public:
    static constexpr std::size_t maxPackets = 96, maxBytes = 1024 * 1024;
    struct Stats {
        std::size_t packets, bytes;
        std::uint64_t replaced, dropped;
    };
    bool push(const unsigned char* p, std::size_t size) {
        if (!p || size < 16 || size > 60001)
            return false;
        std::lock_guard<std::mutex> guard(mutex);
        if (latest(p))
            for (auto it = queue.begin(); it != queue.end(); ++it)
                if (!std::memcmp(it->data(), p, 4)) {
                    bytes -= it->size();
                    queue.erase(it);
                    ++replaced;
                    break;
                }
        while (queue.size() >= maxPackets || bytes + size > maxBytes) {
            auto victim = queue.begin();
            while (victim != queue.end() && urgent(victim->data()))
                ++victim;
            if (victim == queue.end()) {
                ++dropped;
                return false;
            }
            bytes -= victim->size();
            queue.erase(victim);
            ++dropped;
        }
        queue.emplace_back(p, p + size);
        bytes += size;
        return true;
    }
    bool pop(std::vector<unsigned char>& result) {
        std::lock_guard<std::mutex> guard(mutex);
        if (queue.empty())
            return false;
        auto selected = queue.begin();
        for (auto it = queue.begin(); it != queue.end(); ++it)
            if (urgent(it->data())) {
                selected = it;
                break;
            }
        bytes -= selected->size();
        result = std::move(*selected);
        queue.erase(selected);
        return true;
    }
    Stats stats() {
        std::lock_guard<std::mutex> guard(mutex);
        return {queue.size(), bytes, replaced, dropped};
    }
    void clear() {
        std::lock_guard<std::mutex> guard(mutex);
        queue.clear();
        bytes = 0;
        replaced = dropped = 0;
    }
};
