#pragma once
#include "heavymp_traffic_capture.h"
inline bool heavympTrafficLabPacket(const unsigned char* data, size_t bytes, HeavyMpTrafficSample& output,
                                    std::uint32_t& sequence) {
    if (!data || bytes < 64 || bytes > 5516 ||
        (std::memcmp(data, "HIL1", 4) && std::memcmp(data, "HIL2", 4) && std::memcmp(data, "HIL3", 4)))
        return false;
    using heavympTrafficCaptureDetail::value;
    const bool v3 = std::memcmp(data, "HIL3", 4) == 0, v2 = v3 || std::memcmp(data, "HIL2", 4) == 0;
    const auto word = value<std::uint32_t>(data, 12), count = word & 0xffff;
    if (!count || count > 16 || (word & (v3 ? 0x3fff0000 : 0x7fff0000)) || (v3 && bytes < 76) ||
        (!v2 && (word & 0x80000000 || value<std::uint32_t>(data, 60))))
        return false;
    HeavyMpTrafficSample sample;
    sample.nativeId = value<std::uint32_t>(data, 8);
    sample.vehicleType = (word & 0x80000000) ? 255 : 0;
    sample.lookSeed = v2 ? value<float>(data, 60) : 0;
    if (!std::isfinite(sample.lookSeed) || sample.lookSeed < 0 || sample.lookSeed > 1)
        return false;
    for (unsigned i = 0; i < 3; i++) {
        sample.position[i] = value<double>(data, 16 + i * 8);
        if (!std::isfinite(sample.position[i]) || std::abs(sample.position[i]) > 1e7)
            return false;
    }
    double norm = 0;
    for (unsigned i = 0; i < 4; i++) {
        const float v = value<float>(data, 40 + i * 4);
        if (!std::isfinite(v) || std::abs(v) > 1)
            return false;
        sample.quaternion[i] = v;
        norm += v * v;
    }
    if (norm < .9 || norm > 1.1)
        return false;
    for (auto& v : sample.quaternion)
        v = static_cast<float>(v / std::sqrt(norm));
    sample.lights = value<std::uint32_t>(data, 56);
    if (sample.lights > 0xffff)
        return false;
    sample.hasPaintColor = v3 && (word & 0x40000000);
    if (sample.hasPaintColor && sample.vehicleType == 255)
        return false;
    if (v3)
        for (unsigned i = 0; i < 3; i++) {
            const float c = value<float>(data, 64 + i * 4);
            if (!std::isfinite(c) || c < 0 || c > 16 || (!sample.hasPaintColor && c != 0))
                return false;
            sample.paintColor[i] = c;
        }
    size_t offset = v3 ? 76 : 64;
    for (unsigned i = 0; i < count; i++) {
        if (bytes - offset < 24)
            return false;
        HeavyMpAccessory part;
        part.kind = value<std::uint32_t>(data, offset);
        part.flags = value<std::uint32_t>(data, offset + 4);
        const auto length = value<std::uint32_t>(data, offset + 8);
        const size_t header = part.kind == 4 ? 84 : 24;
        if ((part.kind != 1 && part.kind != 3 && part.kind != 4 && part.kind != 5) ||
            bytes - offset < header || !length || length > 256 || length > bytes - offset - header)
            return false;
        for (unsigned j = 0; j < (part.kind == 4 ? 18u : 3u); j++) {
            part.colors[j] = value<float>(data, offset + 12 + j * 4);
            if (!std::isfinite(part.colors[j]) || std::abs(part.colors[j]) > 16)
                return false;
        }
        part.path.assign(reinterpret_cast<const char*>(data + offset + header), length);
        if (!heavympAsset(part.path))
            return false;
        sample.accessories.push_back(std::move(part));
        offset += header + length;
    }
    if (offset != bytes || sample.accessories[0].kind != 1 ||
        (sample.vehicleType == 255 ? sample.accessories[0].path.compare(0, 21, "/def/vehicle/trailer/")
                                   : sample.accessories[0].path.compare(0, 16, "/def/vehicle/ai/")))
        return false;
    sequence = value<std::uint32_t>(data, 4);
    output = std::move(sample);
    return true;
}
