struct TrailerSdk {
    scs_value_dplacement_t placement{};
    std::array<HeavyMpWheelState, 16> wheels{};
    unsigned wheelCount = 0;
    bool have = false, connected = false;
};
std::array<TrailerSdk, 10> indexedTrailers{};
std::array<std::array<std::string, 7>, 10> trailerChannelNames{};
SCSAPI_VOID indexedPlacement(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t context) {
    auto& t = *static_cast<TrailerSdk*>(context);
    t.have = value && value->type == SCS_VALUE_TYPE_dplacement;
    if (t.have)
        t.placement = value->value_dplacement;
}
SCSAPI_VOID indexedConnected(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t context) {
    static_cast<TrailerSdk*>(context)->connected =
        value && value->type == SCS_VALUE_TYPE_bool && value->value_bool.value != 0;
}
SCSAPI_VOID indexedWheel(scs_string_t name, scs_u32_t index, const scs_value_t* value,
                         scs_context_t context) {
    if (index >= 16)
        return;
    auto& t = *static_cast<TrailerSdk*>(context);
    auto& w = t.wheels[index];
    const float v = value && value->type == SCS_VALUE_TYPE_float && std::isfinite(value->value_float.value)
                        ? value->value_float.value
                        : 0;
    if (strstr(name, ".wheel.rotation"))
        w.rotation = v - floorf(v);
    else if (strstr(name, ".wheel.angular_velocity"))
        w.velocity = std::clamp(v, -100.f, 100.f);
    else if (strstr(name, ".wheel.steering"))
        w.steering = std::clamp(v, -.25f, .25f);
    else if (strstr(name, ".wheel.suspension.deflection"))
        w.suspension = std::clamp(v, -2.f, 2.f);
    else if (strstr(name, ".wheel.lift"))
        w.lift = std::clamp(v, 0.f, 1.f);
    if (value)
        t.wheelCount = std::max(t.wheelCount, index + 1);
}
void registerTrailerSegments(const scs_telemetry_init_params_v100_t* api, scs_u32_t flags) {
    indexedTrailers = {};
    for (unsigned i = 1; i < 10; ++i) {
        const std::string prefix = "trailer." + std::to_string(i) + ".";
        auto& names = trailerChannelNames[i];
        names = {prefix + "world.placement", prefix + "connected",
                 prefix + "wheel.rotation",  prefix + "wheel.angular_velocity",
                 prefix + "wheel.steering",  prefix + "wheel.suspension.deflection",
                 prefix + "wheel.lift"};
        api->register_for_channel(names[0].c_str(), SCS_U32_NIL, SCS_VALUE_TYPE_dplacement, flags,
                                  indexedPlacement, &indexedTrailers[i]);
        api->register_for_channel(names[1].c_str(), SCS_U32_NIL, SCS_VALUE_TYPE_bool, flags, indexedConnected,
                                  &indexedTrailers[i]);
        for (unsigned w = 0; w < 14; ++w)
            for (unsigned j = 2; j < 7; ++j)
                api->register_for_channel(names[j].c_str(), w, SCS_VALUE_TYPE_float, flags, indexedWheel,
                                          &indexedTrailers[i]);
    }
}
std::string trailerSegmentsJson() {
    const unsigned count = heavymp_native_local_trailer_count();
    if (count < 2)
        return {};
    std::string result = trailerJson();
    if (result == "null")
        return {};
    result = "[" + result;
    for (unsigned i = 1; i < count; ++i) {
        const auto& t = indexedTrailers[i];
        if (!t.have)
            return {};
        const auto& p = t.placement.position;
        const auto& r = t.placement.orientation;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return {};
        char text[400]{};
        _snprintf_s_l(text, sizeof(text), _TRUNCATE,
                      ",{\"connected\":%s,\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"heading\":%.8f,\"pitch\":%.8f,"
                      "\"roll\":%.8f,\"wheels\":[",
                      jsonLocale, t.connected ? "true" : "false", p.x, p.y, p.z,
                      static_cast<double>(r.heading), static_cast<double>(r.pitch),
                      static_cast<double>(r.roll));
        result += text;
        for (unsigned j = 0; j < t.wheelCount; ++j) {
            const auto& w = t.wheels[j];
            _snprintf_s_l(
                text, sizeof(text), _TRUNCATE,
                "%s{\"rotation\":%.6f,\"velocity\":%.6f,\"steering\":%.6f,\"suspension\":%.6f,\"lift\":%.6f}",
                jsonLocale, j ? "," : "", w.rotation, w.velocity, w.steering, w.suspension, w.lift);
            result += text;
        }
        result += "]}";
    }
    return result + "]";
}
