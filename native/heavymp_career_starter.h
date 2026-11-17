#pragma once
#include <algorithm>
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace HeavyMpCareerStarter {
inline constexpr char savePath[] = "/def/initial_save/normal/game.sii";
inline constexpr char xpPath[] = "/def/economy_data_xp.sii";
inline constexpr unsigned startingLevel = 100;
inline constexpr unsigned startingMoney = 1000000;
inline constexpr auto multiline = std::regex_constants::ECMAScript | std::regex_constants::multiline;

inline bool validLines(const std::string& data) {
    size_t width = 0;
    for (char c : data) {
        if (c == '\n')
            width = 0;
        else if (!c || ++width > 4096)
            return false;
    }
    return true;
}

inline unsigned experienceForStartingLevel(const std::string& data) {
    if (!validLines(data))
        return 0;
    static const std::regex row(
        R"(^[ \t]*level_xp\[([0-9]{0,3})\][ \t]*:[ \t]*([0-9]{1,7})[ \t]*(?:\x23[^\r\n]*)?\r?$)", multiline);
    unsigned count = 0, total = 0, last = 0;
    for (auto it = std::sregex_iterator(data.begin(), data.end(), row); it != std::sregex_iterator(); ++it) {
        if (count == startingLevel)
            break;
        if ((*it)[1].matched && !(*it)[1].str().empty() && std::stoul((*it)[1].str()) != count)
            return 0;
        last = std::stoul((*it)[2].str());
        if (!last)
            return 0;
        total += last;
        ++count;
    }
    return count ? total + (startingLevel - count) * last : 0;
}

inline bool setUnitValue(std::string& text, const char* unit, const char* field, unsigned value,
                         bool minimum) {
    const std::regex header(
        std::string("^[ \\t]*") + unit + R"([ \t]*:[ \t]*[.A-Za-z0-9_]+[ \t]*(?:\r?\n[ \t]*)?\{)", multiline);
    std::smatch unitMatch;
    if (!std::regex_search(text, unitMatch, header) || std::regex_search(unitMatch.suffix().str(), header))
        return false;
    const auto start = static_cast<size_t>(unitMatch.position() + unitMatch.length());
    static const std::regex closing(R"(^[ \t]*\})", multiline);
    const auto end =
        std::regex_iterator<std::string::const_iterator>(text.cbegin() + start, text.cend(), closing);
    if (end == std::sregex_iterator())
        return false;
    const auto body = text.substr(start, end->position());
    const std::regex fieldPattern(std::string("^[ \\t]*") + field + R"([ \t]*:[ \t]*([^\r\n]*))", multiline);
    std::smatch fieldMatch;
    if (!std::regex_search(body, fieldMatch, fieldPattern)) {
        if (minimum)
            return false;
        const auto newline = text.find("\r\n") == std::string::npos ? "\n" : "\r\n";
        text.insert(start, std::string(newline) + "\t" + field + ": " + std::to_string(value));
        return true;
    }
    if (std::regex_search(fieldMatch.suffix().str(), fieldPattern))
        return false;
    static const std::regex number(R"(^(-?[0-9]{1,18})[ \t]*(?:\x23.*)?$)");
    const auto fieldText = fieldMatch[1].str();
    std::smatch digits;
    if (!std::regex_match(fieldText, digits, number))
        return false;
    const auto current = std::stoll(digits[1].str());
    if (!minimum || current < value) {
        text.replace(start + fieldMatch.position(1), digits[1].length(), std::to_string(value));
    }
    return true;
}

inline bool prepareSave(std::string& text, unsigned experience) {
    if (!experience || text.compare(0, 8, "SiiNunit") || !validLines(text))
        return false;
    auto updated = text;
    if (!setUnitValue(updated, "economy", "experience_points", experience, false) ||
        !setUnitValue(updated, "bank", "money_account", startingMoney, true))
        return false;
    text = std::move(updated);
    return true;
}

// ETS2 accepts a stored ZIP with one entry. Only the initial-save file is
// overlaid, so the map's jobs, routes, garages and relative includes still apply.
inline std::vector<unsigned char> saveArchive(const std::string& save) {
    const std::string name = savePath + 1;
    const auto size = static_cast<std::uint32_t>(save.size());
    std::uint32_t crc = 0xffffffff;
    for (unsigned char byte : save) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    crc ^= 0xffffffff;
    std::vector<unsigned char> bytes;
    const auto append = [&](std::uint32_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            bytes.push_back(static_cast<unsigned char>(value >> (i * 8)));
    };
    append(0x04034b50, 4);
    append(20, 2);
    append(0, 4);
    append(0, 4);
    append(crc, 4);
    append(size, 4);
    append(size, 4);
    append(static_cast<unsigned>(name.size()), 2);
    append(0, 2);
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.insert(bytes.end(), save.begin(), save.end());
    const auto directory = static_cast<std::uint32_t>(bytes.size());
    append(0x02014b50, 4);
    append(20, 2);
    append(20, 2);
    append(0, 4);
    append(0, 4);
    append(crc, 4);
    append(size, 4);
    append(size, 4);
    append(static_cast<unsigned>(name.size()), 2);
    append(0, 4);
    append(0, 4);
    append(0, 4);
    append(0, 4);
    bytes.insert(bytes.end(), name.begin(), name.end());
    const auto directorySize = static_cast<std::uint32_t>(bytes.size()) - directory;
    append(0x06054b50, 4);
    append(0, 4);
    append(1, 2);
    append(1, 2);
    append(directorySize, 4);
    append(directory, 4);
    append(0, 2);
    return bytes;
}
} // namespace HeavyMpCareerStarter
