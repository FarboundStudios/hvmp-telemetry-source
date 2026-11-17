#pragma once
#include "scssdk_telemetry.h"
void heavymp_vehicles_prepare(const scs_telemetry_init_params_v100_t* api);
bool heavymp_vehicles_start(const scs_telemetry_init_params_v100_t* api);
void heavymp_vehicles_stop();
void heavymp_vehicles_frame(const scs_value_dplacement_t& placement, bool available, bool paused);
void heavymp_vehicles_event(scs_event_t event, const void* info);
struct HeavyMpRenderStatus {
    bool hooked, camera;
    unsigned drawn;
    const char* mode = "d3d";
};
HeavyMpRenderStatus heavymp_vehicles_status();
