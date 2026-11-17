#pragma once
#include <string>
inline std::string heavyMpLightFamily(const std::string& reference) {
    for (const char* family : {"megane", "ambulance", "escort", "sedan"})
        if (reference ==
            std::string("/def/vehicle/truck/heavymp.") + family + "/engine/sound_engine.soundref")
            return family;
    return {};
}
// References are owned by the game's traffic sound banks, not generated WAVs.
inline const char* heavyMpSirenReference(const std::string& family) {
    if (family == "megane")
        return "/sound/traffic/police_1.soundref";
    if (family == "ambulance")
        return "/sound/traffic/police_2.soundref";
    return nullptr;
}
inline const char* heavyMpLightHornReference(const std::string& family) {
    return family.empty() ? nullptr : "/def/vehicle/truck/heavymp.escort/cabin/horn.soundref";
}
