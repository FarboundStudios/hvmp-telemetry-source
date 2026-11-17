#pragma once
#include <array>
#include "heavymp_appearance.h"
#include "scssdk_telemetry.h"
#include "heavymp_player_projection.h"
struct HeavyMpWheelState {
    float rotation = 0, velocity = 0, steering = 0, suspension = 0, lift = 0;
};
inline bool heavympAudioReference(const std::string& s) {
    if (s.empty())
        return true;
    if (s.size() > 240 || s.compare(0, 19, "/def/vehicle/truck/") || s.size() < 28 ||
        s.substr(s.size() - 9) != ".soundref" || s.find("..") != std::string::npos)
        return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ||
              c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}
static_assert(sizeof(HeavyMpWheelState) == 20);
struct HeavyMpNativePeer {
    std::array<unsigned char, 16> id{};
    double x = 0, y = 0, z = 0, heading = 0, pitch = 0, roll = 0;
    double speed = 0;
    std::array<HeavyMpWheelState, 16> wheels{};
    unsigned wheelCount = 0, lights = 0, controls = 0, horn = 0;
    unsigned long long wheelSampleAt = 0;
    float rpm = 0, brake = 0, throttle = 0, steering = 0;
    float wiperPhase = 0, rearWiperPhase = 0;
    unsigned wiperMode = 0;
    bool haveWiperPhase = false;
    std::array<std::string, 3> audioReferences{};
    int gear = 0;
    std::shared_ptr<const HeavyMpAppearance> appearance;
    // Independently placed trailer segments, bounded to ten members.
    std::shared_ptr<const HeavyMpNativePeer> trailer;
    bool connected = false, zoneGhost = false;
};
// Development bridge, restricted to the executable fingerprint validated locally.
bool heavymp_native_start(scs_log_t log);
void heavymp_native_frame(const HeavyMpNativePeer* peers, unsigned count, bool active);
void heavymp_native_stop();
unsigned heavymp_native_count();
bool heavymp_native_active();
void heavymp_native_traffic_packet(const unsigned char* data, size_t bytes);
bool heavymp_native_traffic_json(std::string* output);
bool heavymp_native_camera(HeavyMpTagCamera* camera);
bool heavymp_native_local_lights(unsigned* lights);
bool heavymp_native_local_horns(unsigned* horns);
bool heavymp_native_local_wipers(float* phase, float* rearPhase, unsigned* mode);
bool heavymp_native_local_audio(std::array<std::string, 3>* references);
void heavymp_native_car_audio(const scs_value_dplacement_t& placement, float rpm, float throttle, float speed,
                              bool running, bool driving);
void heavymp_native_siren_frame(const scs_value_dplacement_t& placement, float speed, bool driving);
void heavymp_native_panel_frame(bool beacon);
void heavymp_native_collision_packet(const unsigned char* packet, size_t bytes);
void heavymp_native_world_packet(const unsigned char* packet, size_t bytes);
bool heavymp_native_local_appearance(HeavyMpAppearance* appearance);
bool heavymp_native_local_trailer_appearance(HeavyMpAppearance* appearance, unsigned segment = 0);
unsigned heavymp_native_local_trailer_count();

void heavymp_native_repair_packet(const unsigned char* packet, size_t bytes);
bool heavymp_native_repair_result(char* id, bool* okay, const char** reason);
void heavymp_native_dispatch_packet(const unsigned char* packet, size_t bytes);
void heavymp_native_dispatch_eligible(bool value);
bool heavymp_native_dispatch_result(char* id, bool* okay, const char** reason, const char** market,
                                    unsigned* units);

void heavymp_native_dispatch_run();
bool heavymp_native_dispatch_unavailable(std::string* out);
