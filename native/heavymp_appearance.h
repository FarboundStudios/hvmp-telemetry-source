#pragma once
#include <array>
#include <vector>
#include <string>
#include <memory>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <locale>
struct HeavyMpAccessory {
    unsigned kind = 0, flags = 0;
    std::string path, text;
    std::array<float, 18> colors{};
    std::vector<std::uint64_t> tokens;
    std::vector<std::string> hookups;
    std::uint64_t cargoId = 0;
    // Only the cargo seed, indices, counts and seven floats; never process addresses.
    std::array<unsigned char, 52> cargoParams{};
};
struct HeavyMpAppearance {
    bool trailer = false;
    std::array<unsigned char, 32> revision{};
    std::string plate;
    std::vector<HeavyMpAccessory> accessories;
};
inline bool heavympText(const std::string& s, size_t max) {
    if (s.size() > max)
        return false;
    // Strict UTF-8, including rejection of overlong encodings and surrogate code points.
    for (size_t i = 0; i < s.size();) {
        unsigned c = static_cast<unsigned char>(s[i++]);
        if (c < 32 || c == 127)
            return false;
        if (c < 128)
            continue;
        unsigned n = 0, v = 0, min = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            n = 1;
            v = c & 31;
            min = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 2;
            v = c & 15;
            min = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3;
            v = c & 7;
            min = 65536;
        } else
            return false;
        if (i + n > s.size())
            return false;
        while (n--) {
            c = static_cast<unsigned char>(s[i++]);
            if ((c & 192) != 128)
                return false;
            v = (v << 6) | (c & 63);
        }
        if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
            return false;
    }
    return true;
}
inline bool heavympAsset(const std::string& s) {
    if (s.size() > 256 || s.compare(0, 13, "/def/vehicle/") || s.size() < 17 ||
        s.substr(s.size() - 4) != ".sii" || s.find("..") != std::string::npos)
        return false;
    for (size_t i = 13; i < s.size(); ++i) {
        const unsigned char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-' || c == '.' || c == '/'))
            return false;
    }
    return s.find("//") == std::string::npos;
}
inline bool heavympCargoParams(const std::array<unsigned char, 52>& p) {
    for (unsigned offset : {8u, 12u, 16u, 36u}) {
        std::uint32_t value = 0;
        memcpy(&value, p.data() + offset, 4);
        if (value > (offset == 36 ? 4096u : 32u))
            return false;
    }
    for (unsigned offset : {20u, 24u, 28u, 32u, 40u, 44u, 48u}) {
        float value = 0;
        memcpy(&value, p.data() + offset, 4);
        if (!std::isfinite(value) || std::abs(value) > 10000.f)
            return false;
    }
    return true;
}
inline bool heavympValidAppearance(const HeavyMpAppearance& a) {
    const std::string coachDirectory = "/def/vehicle/bus/";
    std::string coachRoot;
    for (const auto& x : a.accessories) {
        if (x.path.compare(0, coachDirectory.size(), coachDirectory))
            continue;
        const auto separator = x.path.find('/', coachDirectory.size());
        if (separator == std::string::npos || separator == coachDirectory.size() ||
            x.path.compare(separator, std::string::npos, "/data.sii"))
            continue;
        if (a.trailer || !coachRoot.empty())
            return false;
        coachRoot = x.path.substr(0, separator + 1);
    }
    std::string lightCar;
    for (const auto& x : a.accessories) {
        if (x.path == "/def/vehicle/truck/heavymp.megane/data.sii")
            lightCar = "megane";
        if (x.path == "/def/vehicle/truck/heavymp.escort/data.sii")
            lightCar = "escort";
        if (x.path == "/def/vehicle/truck/heavymp.ambulance/data.sii")
            lightCar = "ambulance";
        if (x.path == "/def/vehicle/truck/heavymp.sedan/data.sii")
            lightCar = "sedan";
    }
    if (!lightCar.empty())
        for (const auto& x : a.accessories)
            for (const char* side : {"f_", "r_"})
                for (const char* part : {"wheel", "tire", "disc", "hub", "nuts", "cover"}) {
                    const std::string prefix = std::string("/def/vehicle/") + side + part + "/";
                    if (x.path.compare(0, prefix.size(), prefix) == 0 &&
                        x.path != prefix + "heavymp_" + lightCar + ".sii")
                        return false;
                }
    bool dolly = false;
    if (a.trailer)
        for (const auto& x : a.accessories)
            if ((x.path.find("dolly") != std::string::npos &&
                 x.path.find("/data.sii") != std::string::npos) ||
                x.path.find("/chassis/dolly") != std::string::npos)
                dolly = true;
    if (!heavympText(a.plate, 2048) || a.accessories.size() < (dolly ? 2u : 7u) || a.accessories.size() > 256)
        return false;
    unsigned paint = 0, cargo = 0;
    for (const auto& x : a.accessories) {
        if (x.kind < 1 || x.kind > 7 || !heavympAsset(x.path))
            return false;
        if (!coachRoot.empty() && (x.path.compare(0, 19, "/def/vehicle/truck/") == 0 ||
                                   (x.path.compare(0, coachDirectory.size(), coachDirectory) == 0 &&
                                    x.path.compare(0, coachRoot.size(), coachRoot) != 0)))
            return false;
        if (x.kind == 7) {
            if (!a.trailer || ++cargo > 1 || !x.cargoId || x.path.compare(0, 19, "/def/vehicle/cargo/") ||
                !heavympCargoParams(x.cargoParams))
                return false;
        }
        if (x.kind == 2 && !heavympText(x.text, 2048))
            return false;
        const unsigned n = x.kind == 4 ? 18 : (x.kind == 3 || x.kind == 5 ? 3 : 0);
        for (unsigned j = 0; j < n; ++j)
            if (!std::isfinite(x.colors[j]) || x.colors[j] < -1 || x.colors[j] > 16)
                return false;
        if (x.kind == 4)
            ++paint;
        if (x.kind == 5) {
            if (x.tokens.size() > 256 || x.tokens.size() != x.hookups.size())
                return false;
            for (const auto& s : x.hookups) {
                if (!heavympText(s, 128))
                    return false;
                for (unsigned char c : s)
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '.' || c == '-'))
                        return false;
            }
        }
    }
    if (dolly ? paint > 1 : paint != 1)
        return false;
    const std::vector<const char*> tails =
        dolly       ? std::vector<const char*>{"/data.sii", "/chassis/"}
        : a.trailer ? std::vector<const char*>{"/data.sii", "/chassis/", "/body/", "/paint_job/"}
                    : std::vector<const char*>{"/data.sii", "/chassis/",      "/cabin/",    "/interior/",
                                               "/engine/",  "/transmission/", "/paint_job/"};
    for (const char* tail : tails) {
        bool found = false;
        for (const auto& x : a.accessories)
            if (coachRoot.empty() ? x.path.find(tail) != std::string::npos
                                  : x.path.compare(0, coachRoot.size(), coachRoot) == 0 &&
                                        x.path.compare(coachRoot.size(), strlen(tail + 1), tail + 1) == 0)
                found = true;
        if (!found)
            return false;
    }
    return true;
}
inline std::shared_ptr<const HeavyMpAppearance>
heavympAppearancePacket(const unsigned char* data, size_t length, std::array<unsigned char, 16>& id) {
    if (length < 60 || length > 60000 || (memcmp(data, "HMC1", 4) && memcmp(data, "HMC2", 4)))
        return {};
    const unsigned char* cursor = data + 52;
    const auto* end = data + length;
    auto read = [&](auto& v) {
        if (static_cast<size_t>(end - cursor) < sizeof(v))
            return false;
        memcpy(&v, cursor, sizeof(v));
        cursor += sizeof(v);
        return true;
    };
    auto text = [&](std::string& s, unsigned n) {
        if (static_cast<size_t>(end - cursor) < n)
            return false;
        s.assign(reinterpret_cast<const char*>(cursor), n);
        cursor += n;
        return true;
    };
    std::uint32_t count = 0;
    std::uint16_t plate = 0, reserved = 0;
    if (!read(count) || !read(plate) || !read(reserved) || reserved ||
        count < (memcmp(data, "HMC2", 4) ? 7u : 2u) || count > 256 || plate > 2048)
        return {};
    auto a = std::make_shared<HeavyMpAppearance>();
    a->trailer = memcmp(data, "HMC2", 4) == 0;
    memcpy(id.data(), data + 4, 16);
    memcpy(a->revision.data(), data + 20, 32);
    if (!text(a->plate, plate))
        return {};
    for (unsigned i = 0; i < count; ++i) {
        HeavyMpAccessory x;
        unsigned char kind = 0, r = 0;
        std::uint16_t path = 0, txt = 0, tokens = 0, hookups = 0, payload = 0;
        if (!read(kind) || !read(r) || !read(path) || !read(txt) || !read(tokens) || !read(hookups) ||
            !read(payload) || r || kind < 1 || kind > 7 || path > 256 || txt > 2048 || tokens > 256 ||
            hookups > 256)
            return {};
        x.kind = kind;
        const unsigned expected = kind == 3 ? 16 : kind == 4 ? 72 : kind == 5 ? 12 : kind == 7 ? 60 : 0;
        if (payload != expected || (kind != 2 && txt) || (kind != 5 && (tokens || hookups)) ||
            tokens != hookups || !text(x.path, path) || !text(x.text, txt))
            return {};
        if (kind == 3 && !read(x.flags))
            return {};
        const unsigned n = kind == 4 ? 18 : (kind == 3 || kind == 5 ? 3 : 0);
        for (unsigned j = 0; j < n; ++j)
            if (!read(x.colors[j]))
                return {};
        if (kind == 7) {
            if (!read(x.cargoId))
                return {};
            for (auto& byte : x.cargoParams)
                if (!read(byte))
                    return {};
        }
        for (unsigned j = 0; j < tokens; ++j) {
            std::uint64_t t = 0;
            if (!read(t))
                return {};
            x.tokens.push_back(t);
        }
        for (unsigned j = 0; j < hookups; ++j) {
            std::uint16_t size = 0;
            std::string s;
            if (!read(size) || size > 128 || !text(s, size))
                return {};
            x.hookups.push_back(std::move(s));
        }
        a->accessories.push_back(std::move(x));
    }
    return cursor == end && heavympValidAppearance(*a) ? a : nullptr;
}
inline std::string heavympAppearanceJson(const HeavyMpAppearance& a) {
    if (!heavympValidAppearance(a))
        return {};
    auto quote = [](const std::string& s) {
        std::string r = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\')
                r += '\\';
            r += c;
        }
        return r + '"';
    };
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << "{\"plate\":" << quote(a.plate) << ",\"accessories\":[";
    bool first = true;
    for (const auto& x : a.accessories) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"kind\":" << x.kind << ",\"path\":" << quote(x.path);
        if (x.kind == 2)
            out << ",\"text\":" << quote(x.text);
        if (x.kind == 3)
            out << ",\"flags\":" << x.flags;
        if (x.kind == 7) {
            out << ",\"cargoId\":\"" << std::hex << std::setw(16) << std::setfill('0') << x.cargoId
                << "\",\"cargoParams\":\"";
            for (auto byte : x.cargoParams)
                out << std::setw(2) << unsigned(byte);
            out << std::dec << '"';
        }
        const unsigned n = x.kind == 4 ? 18 : (x.kind == 3 || x.kind == 5 ? 3 : 0);
        if (n) {
            out << ",\"colors\":[";
            for (unsigned j = 0; j < n; ++j) {
                if (j)
                    out << ',';
                out << x.colors[j];
            }
            out << ']';
        }
        if (x.kind == 5) {
            out << ",\"tokens\":[";
            for (size_t j = 0; j < x.tokens.size(); ++j) {
                if (j)
                    out << ',';
                out << '"' << std::hex << std::setw(16) << std::setfill('0') << x.tokens[j] << std::dec
                    << '"';
            }
            out << "],\"hookups\":[";
            for (size_t j = 0; j < x.hookups.size(); ++j) {
                if (j)
                    out << ',';
                out << quote(x.hookups[j]);
            }
            out << ']';
        }
        out << '}';
    }
    out << "]}";
    const auto result = out.str();
    return result.size() <= 48000 ? result : std::string{};
}
