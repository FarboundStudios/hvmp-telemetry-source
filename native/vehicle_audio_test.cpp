#include "heavymp_vehicle_audio.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main() {
    assert(heavyMpLightFamily("/def/vehicle/truck/heavymp.sedan/engine/sound_engine.soundref") == "sedan");
    assert(heavyMpLightFamily("/def/vehicle/truck/scania.r/engine/sound_engine.soundref").empty());
    assert(std::strcmp(heavyMpSirenReference("megane"), "/sound/traffic/police_1.soundref") == 0);
    assert(std::strcmp(heavyMpSirenReference("ambulance"), "/sound/traffic/police_2.soundref") == 0);
    assert(!heavyMpSirenReference("escort") && !heavyMpSirenReference("sedan"));
    assert(heavyMpLightHornReference("sedan") && !heavyMpLightHornReference(""));
    puts("PASS: stock police/ambulance routes separated; civilian vehicles have no siren.");
}
