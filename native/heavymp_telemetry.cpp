#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <locale.h>
#include <initializer_list>
#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_job_common_channels.h"
#include "heavymp_vehicles.h"
#include "heavymp_native.h"
#include "heavymp_content.h"
#include "heavymp_escort_panel_state.h"

namespace {
SOCKET output = INVALID_SOCKET;
sockaddr_in destination{};
scs_value_dplacement_t placement{};
float speed = 0;
std::array<HeavyMpWheelState, 16> wheels{};
unsigned wheelCount = 0, trailerWheelCount = 0;
std::array<HeavyMpWheelState, 16> trailerWheels{};
scs_value_dplacement_t trailerPlacement{};
bool haveTrailer = false, trailerConnected = false;
float rpm = 0, brake = 0, throttle = 0, steering = 0;
int gear = 0;
bool engineOn = false, parkingBrake = false, wipers = false, beacon = false;
std::string brand, model, plate;
bool havePlacement = false, paused = true, winsockStarted = false;
bool sessionActive = false, sessionTried = false;
scs_telemetry_init_params_v100_t sessionApi{};
ULONGLONG lastSent = 0;
ULONGLONG lastAppearance = 0;
_locale_t jsonLocale = nullptr;
std::string jsonQuote(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\')
            result += '\\';
        if (c >= 32)
            result += static_cast<char>(c);
    }
    return result + '"';
}
#include "heavymp_dispatch_telemetry.inl"
std::string trailerJson() {
    if (!haveTrailer)
        return "null";
    const auto& p = trailerPlacement.position;
    const auto& r = trailerPlacement.orientation;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(r.heading) ||
        !std::isfinite(r.pitch) || !std::isfinite(r.roll))
        return "null";
    char text[400]{};
    _snprintf_s_l(text, sizeof(text), _TRUNCATE,
                  "{\"connected\":%s,\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"heading\":%.8f,\"pitch\":%.8f,"
                  "\"roll\":%.8f,\"wheels\":[",
                  jsonLocale, trailerConnected ? "true" : "false", p.x, p.y, p.z,
                  static_cast<double>(r.heading), static_cast<double>(r.pitch), static_cast<double>(r.roll));
    std::string result = text;
    for (unsigned i = 0; i < std::min(trailerWheelCount, 16u); ++i) {
        const auto& w = trailerWheels[i];
        _snprintf_s_l(
            text, sizeof(text), _TRUNCATE,
            "%s{\"rotation\":%.6f,\"velocity\":%.6f,\"steering\":%.6f,\"suspension\":%.6f,\"lift\":%.6f}",
            jsonLocale, i ? "," : "", w.rotation, w.velocity, w.steering, w.suspension, w.lift);
        result += text;
    }
    return result + "]}";
}
#include "heavymp_trailer_sdk.inl"
std::string truckJson() {
    unsigned lights = 0;
    heavymp_native_local_lights(&lights);
    // The escort sign is retired; do not publish its former animation flag.
    lights &= ~heavyMpPanelLightBit;
    char text[420]{};
    _snprintf_s_l(text, sizeof(text), _TRUNCATE,
                  "{\"lights\":%u,\"controls\":{\"engine\":%s,\"parkingBrake\":%s,\"wipers\":%s,\"rpm\":%.3f,"
                  "\"gear\":%d,\"brake\":%.5f,\"throttle\":%.5f,\"steering\":%.5f},\"identity\":{",
                  jsonLocale, lights, engineOn ? "true" : "false", parkingBrake ? "true" : "false",
                  wipers ? "true" : "false", rpm, gear, brake, throttle, steering);
    std::string result = text;
    float wiperPhase = 0, rearWiperPhase = 0;
    unsigned wiperMode = 0;
    if (!paused && heavymp_native_local_wipers(&wiperPhase, &rearWiperPhase, &wiperMode)) {
        const auto endControls = result.find("},\"identity\":{");
        _snprintf_s_l(text, sizeof(text), _TRUNCATE,
                      ",\"wiperPhase\":%.6f,\"rearWiperPhase\":%.6f,\"wiperMode\":%u", jsonLocale, wiperPhase,
                      rearWiperPhase, wiperMode);
        if (endControls != std::string::npos)
            result.insert(endControls, text);
    }
    unsigned horns = 0;
    if (!paused)
        heavymp_native_local_horns(&horns);
    std::array<std::string, 3> audio{};
    if (heavymp_native_local_audio(&audio)) {
        const auto at = result.find("\"identity\":{");
        if (at != std::string::npos)
            result.insert(at, "\"audio\":[" + jsonQuote(audio[0]) + "," + jsonQuote(audio[1]) + "," +
                                  jsonQuote(audio[2]) + "],");
    }
    // Insert the horn state before the open identity object.
    const auto identity = result.find("\"identity\":{");
    if (identity != std::string::npos)
        result.insert(identity, std::string("\"horn\":{\"normal\":") + ((horns & 1) ? "true" : "false") +
                                    ",\"air\":" + ((horns & 2) ? "true" : "false") + "},");
    result += "\"brand\":" + jsonQuote(brand) + ",\"model\":" + jsonQuote(model) +
              ",\"plate\":" + jsonQuote(plate) + "},\"wheels\":[";
    for (unsigned i = 0; i < std::min(wheelCount, 16u); ++i) {
        const auto& w = wheels[i];
        _snprintf_s_l(
            text, sizeof(text), _TRUNCATE,
            "%s{\"rotation\":%.6f,\"velocity\":%.6f,\"steering\":%.6f,\"suspension\":%.6f,\"lift\":%.6f}",
            jsonLocale, i ? "," : "", w.rotation, w.velocity, w.steering, w.suspension, w.lift);
        result += text;
    }
    return result + "]}";
}

void cleanup() {
    heavymp_vehicles_stop();
    if (output != INVALID_SOCKET) {
        closesocket(output);
        output = INVALID_SOCKET;
    }
    if (winsockStarted) {
        WSACleanup();
        winsockStarted = false;
    }
    if (jsonLocale) {
        _free_locale(jsonLocale);
        jsonLocale = nullptr;
    }
    havePlacement = false;
    sessionActive = false;
}

#include "heavymp_session.inl"
bool requestedSession() {
    logLauncherUiResult();
    const auto ui = GetModuleHandleW(L"heavymp-ui.dll");
    if (!ui)
        return false;
    using Active = DWORD(WINAPI*)();
    const auto active = reinterpret_cast<Active>(GetProcAddress(ui, "HeavyMpUiSessionActive"));
    return active && active() == 1;
}
bool activateSession() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return false;
    winsockStarted = true;
    jsonLocale = _create_locale(LC_ALL, "C");
    output = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (output == INVALID_SOCKET || !jsonLocale) {
        cleanup();
        return false;
    }
    u_long nonBlocking = 1;
    if (ioctlsocket(output, FIONBIO, &nonBlocking) != 0) {
        cleanup();
        return false;
    }
    destination = {};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    unsigned short localPort = 34061;
    char portBuffer[16]{};
    const DWORD size = GetEnvironmentVariableA("HEAVYMP_TELEMETRY_PORT", portBuffer, sizeof(portBuffer));
    if (size > 0 && size < sizeof(portBuffer)) {
        char* end = nullptr;
        const long value = strtol(portBuffer, &end, 10);
        if (end && !*end && value > 0 && value <= 65535)
            localPort = static_cast<unsigned short>(value);
    }
    destination.sin_port = htons(localPort);
    sessionActive = true;
    heavymp_vehicles_start(&sessionApi);
    if (sessionApi.common.log)
        sessionApi.common.log(
            SCS_LOG_TYPE_message,
            "[HeavyMP] Launcher session activated by UI; telemetry and multiplayer enabled.");
    return true;
}
void ensureSession() {
    const bool requested = requestedSession();
    if (!requested) {
        if (sessionActive)
            cleanup();
        sessionTried = false;
        return;
    }
    if (!sessionActive && !sessionTried) {
        sessionTried = true;
        if (!activateSession() && sessionApi.common.log)
            sessionApi.common.log(SCS_LOG_TYPE_error,
                                  "[HeavyMP] Session activation failed; no multiplayer started.");
    }
}

void transmit(bool force = false) {
    const auto now = GetTickCount64();
    if (!havePlacement || output == INVALID_SOCKET || (!force && now - lastSent < 50))
        return;
    const auto& p = placement.position;
    const auto& r = placement.orientation;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(r.heading) ||
        !std::isfinite(r.pitch) || !std::isfinite(r.roll) || !std::isfinite(speed))
        return;
    char buffer[60000];
    const auto render = heavymp_vehicles_status();
    const auto truck = truckJson(), trailer = trailerJson(), segments = trailerSegmentsJson();
    const auto chain = segments.empty() ? std::string{} : std::string(",\"trailers\":") + segments;
    // SDK orientation values are turns, not degrees. Preserve them on the wire.
    const int length =
        _snprintf_s_l(buffer, sizeof(buffer), _TRUNCATE,
                      "{\"protocol\":1,\"type\":\"telemetry\",\"gameId\":\"eut2\",\"state\":{"
                      "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"heading\":%.8f,\"pitch\":%.8f,\"roll\":%.8f,"
                      "\"speed\":%.6f,\"paused\":%s,\"truck\":%s,\"trailer\":%s%s},\"render\":{\"hooked\":%s,"
                      "\"camera\":%s,\"drawn\":%u,\"mode\":\"%s\"}}",
                      jsonLocale, p.x, p.y, p.z, static_cast<double>(r.heading), static_cast<double>(r.pitch),
                      static_cast<double>(r.roll), static_cast<double>(speed), paused ? "true" : "false",
                      truck.c_str(), trailer.c_str(), chain.c_str(), render.hooked ? "true" : "false",
                      render.camera ? "true" : "false", render.drawn, render.mode);
    if (length <= 0 || length >= static_cast<int>(sizeof(buffer)))
        return;
    // Non-blocking loopback only: never do DNS or contact the VPS on the game thread.
    sendto(output, buffer, length, 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    dispatchLiveSupported = heavymp_native_active();
    dispatchTransmit();
    static ULONGLONG companiesSentAt = 0;
    if (now - companiesSentAt >= 10000) {
        companiesSentAt = now;
        std::string excluded;
        if (heavymp_native_dispatch_unavailable(&excluded)) {
            const std::string packet =
                "{\"protocol\":1,\"gameId\":\"eut2\",\"type\":\"dispatch_companies\",\"unavailable\":" +
                excluded + "}";
            sendto(output, packet.c_str(), static_cast<int>(packet.size()), 0,
                   reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        }
    }
    heavymp_native_dispatch_eligible(sessionActive && !paused && std::abs(speed) < 0.0027f &&
                                     !dispatchJob.active);
    lastSent = now;
    static ULONGLONG trafficSentAt = 0;
    if (now - trafficSentAt >= 100) {
        trafficSentAt = now;
        std::string packet;
        if (heavymp_native_traffic_json(&packet) && packet.size() < 60000)
            sendto(output, packet.c_str(), static_cast<int>(packet.size()), 0,
                   reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    }
    char repairId[37]{};
    bool repairOkay = false;
    const char* repairReason = "";
    if (heavymp_native_repair_result(repairId, &repairOkay, &repairReason)) {
        const int resultLength = sprintf_s(buffer,
                                           "{\"protocol\":1,\"gameId\":\"eut2\",\"type\":\"command_result\","
                                           "\"id\":\"%s\",\"ok\":%s,\"reason\":\"%s\"}",
                                           repairId, repairOkay ? "true" : "false", repairReason);
        if (resultLength > 0)
            sendto(output, buffer, resultLength, 0, reinterpret_cast<sockaddr*>(&destination),
                   sizeof(destination));
    }
    char dispatchId[37]{};
    bool dispatchOkay = false;
    const char *dispatchReason = "", *dispatchMarket = "freight_market";
    unsigned dispatchUnits = 0;
    if (heavymp_native_dispatch_result(dispatchId, &dispatchOkay, &dispatchReason, &dispatchMarket,
                                       &dispatchUnits)) {
        const std::string reply =
            std::string("{\"protocol\":1,\"gameId\":\"eut2\",\"type\":\"dispatch_result\",\"id\":") +
            jsonQuote(dispatchId) + ",\"ok\":" + (dispatchOkay ? "true" : "false") +
            ",\"reason\":" + jsonQuote(dispatchReason) + ",\"market\":" + jsonQuote(dispatchMarket) +
            ",\"unitsCount\":" + std::to_string(dispatchUnits) + "}";
        sendto(output, reply.c_str(), static_cast<int>(reply.size()), 0,
               reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    }
    if (!paused && now - lastAppearance >= 1000) {
        lastAppearance = now;
        HeavyMpAppearance trailerAppearance;
        const unsigned segmentCount = heavymp_native_local_trailer_count();
        for (unsigned segment = 0; haveTrailer && segment < segmentCount; ++segment)
            if (heavymp_native_local_trailer_appearance(&trailerAppearance, segment)) {
                const auto json = heavympAppearanceJson(trailerAppearance);
                if (!json.empty()) {
                    const auto message =
                        std::string("{\"protocol\":1,\"type\":\"trailer_configuration\",\"gameId\":\"eut2\","
                                    "\"segment\":") +
                        std::to_string(segment) + ",\"segmentCount\":" + std::to_string(segmentCount) +
                        ",\"generation\":" + std::to_string(lastAppearance) + ",\"appearance\":" + json + "}";
                    sendto(output, message.c_str(), static_cast<int>(message.size()), 0,
                           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
                }
            }
        HeavyMpAppearance appearance;
        if (heavymp_native_local_appearance(&appearance)) {
            const auto json = heavympAppearanceJson(appearance);
            if (!json.empty()) {
                const auto message =
                    std::string(
                        "{\"protocol\":1,\"type\":\"configuration\",\"gameId\":\"eut2\",\"appearance\":") +
                    json + "}";
                sendto(output, message.c_str(), static_cast<int>(message.size()), 0,
                       reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
            }
        }
    }
}

SCSAPI_VOID positionCallback(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t) {
    havePlacement = value && value->type == SCS_VALUE_TYPE_dplacement;
    if (havePlacement)
        placement = value->value_dplacement;
}
SCSAPI_VOID trailerPositionCallback(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t) {
    haveTrailer = value && value->type == SCS_VALUE_TYPE_dplacement;
    if (haveTrailer)
        trailerPlacement = value->value_dplacement;
}
SCSAPI_VOID speedCallback(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t) {
    speed = value && value->type == SCS_VALUE_TYPE_float ? value->value_float.value : 0;
}
SCSAPI_VOID scalarCallback(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t context) {
    *static_cast<float*>(context) =
        value && value->type == SCS_VALUE_TYPE_float && std::isfinite(value->value_float.value)
            ? value->value_float.value
            : 0;
}
SCSAPI_VOID booleanCallback(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t context) {
    *static_cast<bool*>(context) =
        value && value->type == SCS_VALUE_TYPE_bool && value->value_bool.value != 0;
}
SCSAPI_VOID gearCallback(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t) {
    gear = value && value->type == SCS_VALUE_TYPE_s32 ? value->value_s32.value : 0;
}
SCSAPI_VOID wheelCallback(scs_string_t name, scs_u32_t index, const scs_value_t* value, scs_context_t) {
    if (index >= 16)
        return;
    const float v = value && value->type == SCS_VALUE_TYPE_float && std::isfinite(value->value_float.value)
                        ? value->value_float.value
                        : 0;
    const bool trailer = !strncmp(name, "trailer.", 8);
    auto& w = trailer ? trailerWheels[index] : wheels[index];
    if ((!strcmp(name, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_rotation) ||
         !strcmp(name, SCS_TELEMETRY_TRAILER_CHANNEL_wheel_rotation)))
        w.rotation = v - floorf(v);
    else if ((!strcmp(name, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_velocity) ||
              !strcmp(name, SCS_TELEMETRY_TRAILER_CHANNEL_wheel_velocity)))
        w.velocity = std::clamp(v, -100.f, 100.f);
    else if ((!strcmp(name, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_steering) ||
              !strcmp(name, SCS_TELEMETRY_TRAILER_CHANNEL_wheel_steering)))
        w.steering = std::clamp(v, -.25f, .25f);
    else if ((!strcmp(name, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_susp_deflection) ||
              !strcmp(name, SCS_TELEMETRY_TRAILER_CHANNEL_wheel_susp_deflection)))
        w.suspension = std::clamp(v, -2.f, 2.f);
    else if ((!strcmp(name, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_lift) ||
              !strcmp(name, SCS_TELEMETRY_TRAILER_CHANNEL_wheel_lift)))
        w.lift = std::clamp(v, 0.f, 1.f);
    if (value) {
        auto& count = trailer ? trailerWheelCount : wheelCount;
        count = std::max(count, index + 1);
    }
}
void truckConfiguration(const void* info) {
    if (!info)
        return;
    const auto* c = static_cast<const scs_telemetry_configuration_t*>(info);
    unsigned segmentIndex = 0;
    if (sscanf_s(c->id, "trailer.%u", &segmentIndex) == 1 && segmentIndex > 0 && segmentIndex < 10) {
        auto& t = indexedTrailers[segmentIndex];
        t = {};
        for (auto a = c->attributes; a && a->name; ++a)
            if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_count) &&
                a->value.type == SCS_VALUE_TYPE_u32)
                t.wheelCount = std::min(a->value.value_u32.value, 16u);
        return;
    }
    if (!strcmp(c->id, "trailer") || !strcmp(c->id, "trailer.0")) {
        trailerWheelCount = 0;
        trailerWheels = {};
        haveTrailer = false;
        for (auto a = c->attributes; a && a->name; ++a)
            if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_count) &&
                a->value.type == SCS_VALUE_TYPE_u32)
                trailerWheelCount = std::min(a->value.value_u32.value, 16u);
        return;
    }
    if (dispatchConfig(c))
        return;
    if (strcmp(c->id, SCS_TELEMETRY_CONFIG_truck))
        return;
    heavymp_native_siren_frame(placement, speed, false);
    heavymp_native_car_audio(placement, rpm, throttle, speed, engineOn, false);
    wheelCount = 0;
    wheels = {};
    brand.clear();
    model.clear();
    plate.clear();
    for (auto a = c->attributes; a && a->name; ++a) {
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_count) &&
            a->value.type == SCS_VALUE_TYPE_u32)
            wheelCount = std::min(a->value.value_u32.value, 16u);
        if (a->value.type != SCS_VALUE_TYPE_string || !a->value.value_string.value)
            continue;
        const std::string value = a->value.value_string.value;
        if (value.size() > 64)
            continue;
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand_id))
            brand = value;
        else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id))
            model = value;
        else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_license_plate))
            plate = value;
    }
}
SCSAPI_VOID eventCallback(scs_event_t event, const void* info, scs_context_t) {
    ensureSession();
    if (event == SCS_TELEMETRY_EVENT_gameplay)
        dispatchGameplayEvent(info);
    heavymp_vehicles_event(event, info);
    if (event == SCS_TELEMETRY_EVENT_configuration)
        truckConfiguration(info);
    if (event == SCS_TELEMETRY_EVENT_started) {
        paused = false;
        havePlacement = false;
        lastSent = 0;
    } else if (event == SCS_TELEMETRY_EVENT_paused) {
        paused = true;
        heavymp_native_car_audio(placement, rpm, throttle, speed, engineOn, false);
        heavymp_native_siren_frame(placement, speed, false);
        transmit(true);
    } else if (event == SCS_TELEMETRY_EVENT_frame_end) {
        heavymp_native_dispatch_eligible(sessionActive && havePlacement && !paused &&
                                         std::abs(speed) < 0.0027f && !dispatchJob.active);
        heavymp_native_dispatch_run();
        heavymp_vehicles_frame(placement, havePlacement, paused);
        heavymp_native_car_audio(placement, rpm, throttle, speed, engineOn,
                                 sessionActive && havePlacement && !paused);
        heavymp_native_siren_frame(placement, speed, sessionActive && havePlacement && !paused);
        transmit();
    }
}
} // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}

SCSAPI_RESULT scs_telemetry_init(scs_u32_t version, const scs_telemetry_init_params_t* params) {
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01)
        return SCS_RESULT_unsupported;
    if (!params)
        return SCS_RESULT_invalid_parameter;
    const auto* api = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    if (!api->common.game_id || std::strcmp(api->common.game_id, "eut2") != 0)
        return SCS_RESULT_unsupported;
    if (!api->register_for_event || !api->register_for_channel)
        return SCS_RESULT_invalid_parameter;
    // Synchronous mount before the selected profile/map loads. No fresh launcher
    // request means a no-op, so an ordinary solo startup remains unchanged.
    if (heavymp_content_initialize(api->common.log) == 2)
        return SCS_RESULT_generic_error;
    stopSessionRequests();
    cleanup();
    sessionApi = *api;
    sessionTried = false;
    paused = true;
    lastSent = 0;
    lastAppearance = 0;
    speed = 0;
    haveTrailer = false;
    trailerConnected = false;
    trailerWheelCount = 0;
    trailerWheels = {};
    wheels = {};
    wheelCount = 0;
    rpm = brake = throttle = steering = 0;
    gear = 0;
    engineOn = parkingBrake = wipers = beacon = false;
    brand.clear();
    model.clear();
    plate.clear();
    bool ok = true;
    for (auto event : {SCS_TELEMETRY_EVENT_started, SCS_TELEMETRY_EVENT_paused, SCS_TELEMETRY_EVENT_frame_end,
                       SCS_TELEMETRY_EVENT_configuration, SCS_TELEMETRY_EVENT_gameplay}) {
        ok = ok && api->register_for_event(event, eventCallback, nullptr) == SCS_RESULT_ok;
    }
    const scs_u32_t flags = SCS_TELEMETRY_CHANNEL_FLAG_each_frame | SCS_TELEMETRY_CHANNEL_FLAG_no_value;
    registerTrailerSegments(api, flags);
    ok = ok && api->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_U32_NIL,
                                         SCS_VALUE_TYPE_dplacement, flags, positionCallback,
                                         nullptr) == SCS_RESULT_ok;
    ok = ok && api->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                                         flags, speedCallback, nullptr) == SCS_RESULT_ok;
    if (!ok) {
        cleanup();
        return SCS_RESULT_generic_error;
    }
    api->register_for_channel(SCS_TELEMETRY_TRAILER_CHANNEL_world_placement, SCS_U32_NIL,
                              SCS_VALUE_TYPE_dplacement, flags, trailerPositionCallback, nullptr);
    api->register_for_channel(SCS_TELEMETRY_TRAILER_CHANNEL_connected, SCS_U32_NIL, SCS_VALUE_TYPE_bool,
                              flags, booleanCallback, &trailerConnected);
    for (unsigned i = 0; i < 14; ++i)
        for (const auto channel :
             {SCS_TELEMETRY_TRAILER_CHANNEL_wheel_rotation, SCS_TELEMETRY_TRAILER_CHANNEL_wheel_velocity,
              SCS_TELEMETRY_TRAILER_CHANNEL_wheel_steering,
              SCS_TELEMETRY_TRAILER_CHANNEL_wheel_susp_deflection, SCS_TELEMETRY_TRAILER_CHANNEL_wheel_lift})
            api->register_for_channel(channel, i, SCS_VALUE_TYPE_float, flags, wheelCallback, nullptr);
    // Optional channels do not prevent telemetry on a build lacking a field.
    for (unsigned i = 0; i < 14; ++i)
        for (const auto channel :
             {SCS_TELEMETRY_TRUCK_CHANNEL_wheel_rotation, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_velocity,
              SCS_TELEMETRY_TRUCK_CHANNEL_wheel_steering, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_susp_deflection,
              SCS_TELEMETRY_TRUCK_CHANNEL_wheel_lift})
            api->register_for_channel(channel, i, SCS_VALUE_TYPE_float, flags, wheelCallback, nullptr);
    for (const auto channel : {std::pair<const char*, float*>{SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm, &rpm},
                               {SCS_TELEMETRY_TRUCK_CHANNEL_effective_brake, &brake},
                               {SCS_TELEMETRY_TRUCK_CHANNEL_effective_throttle, &throttle},
                               {SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering, &steering}})
        api->register_for_channel(channel.first, SCS_U32_NIL, SCS_VALUE_TYPE_float, flags, scalarCallback,
                                  channel.second);
    for (const auto channel :
         {std::pair<const char*, bool*>{SCS_TELEMETRY_TRUCK_CHANNEL_engine_enabled, &engineOn},
          {SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake, &parkingBrake},
          {SCS_TELEMETRY_TRUCK_CHANNEL_wipers, &wipers},
          {SCS_TELEMETRY_TRUCK_CHANNEL_light_beacon, &beacon}})
        api->register_for_channel(channel.first, SCS_U32_NIL, SCS_VALUE_TYPE_bool, flags, booleanCallback,
                                  channel.second);
    api->register_for_channel(SCS_TELEMETRY_JOB_CHANNEL_cargo_damage, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                              flags, scalarCallback, &dispatchJob.cargoDamage);
    api->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear, SCS_U32_NIL, SCS_VALUE_TYPE_s32, flags,
                              gearCallback, nullptr);
    heavymp_vehicles_prepare(api);
    if (api->common.log)
        api->common.log(SCS_LOG_TYPE_message,
                        "[HeavyMP] Plugin loaded. Multiplayer requires launch from HeavyMP.");
    if (!startSessionRequests()) {
        if (api->common.log)
            api->common.log(SCS_LOG_TYPE_error,
                            "[HeavyMP session] Could not start launcher request listener.");
        cleanup();
        return SCS_RESULT_generic_error;
    }
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown() {
    stopSessionRequests();
    cleanup();
}
