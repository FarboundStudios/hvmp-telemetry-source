// Read-only official SDK job data. No game-memory writes or save manipulation.
struct DispatchJobTelemetry {
    bool active = false;
    std::string cargo, sourceCompany, sourceCity, destinationCompany, destinationCity;
    float cargoDamage = 0;
    double mass = 0, income = 0, distanceKm = 0;
};
DispatchJobTelemetry dispatchJob, dispatchFinishedJob, dispatchLastActiveJob;
bool dispatchLiveSupported = false;
std::string dispatchGameplay, dispatchEventId;
ULONGLONG dispatchJobSent = 0, dispatchEventAt = 0;
double dispatchRevenue = 0, dispatchXp = 0, dispatchDistance = 0;
double dispatchNumber(const scs_value_t& value) {
    switch (value.type) {
    case SCS_VALUE_TYPE_float:
        return value.value_float.value;
    case SCS_VALUE_TYPE_double:
        return value.value_double.value;
    case SCS_VALUE_TYPE_u32:
        return value.value_u32.value;
    case SCS_VALUE_TYPE_s32:
        return value.value_s32.value;
    case SCS_VALUE_TYPE_u64:
        return static_cast<double>(value.value_u64.value);
    case SCS_VALUE_TYPE_s64:
        return static_cast<double>(value.value_s64.value);
    default:
        return 0;
    }
}
bool dispatchConfig(const scs_telemetry_configuration_t* config) {
    if (!config || strcmp(config->id, "job"))
        return false;
    dispatchJob = {};
    for (auto a = config->attributes; a && a->name; ++a) {
        if (a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value) {
            std::string value = a->value.value_string.value;
            if (value.size() > 100)
                continue;
            if (!strcmp(a->name, "cargo.id"))
                dispatchJob.cargo = value;
            else if (!strcmp(a->name, "source.company.id"))
                dispatchJob.sourceCompany = value;
            else if (!strcmp(a->name, "source.city.id"))
                dispatchJob.sourceCity = value;
            else if (!strcmp(a->name, "destination.company.id"))
                dispatchJob.destinationCompany = value;
            else if (!strcmp(a->name, "destination.city.id"))
                dispatchJob.destinationCity = value;
        } else {
            const double value = dispatchNumber(a->value);
            if (!std::isfinite(value))
                continue;
            if (!strcmp(a->name, "cargo.mass"))
                dispatchJob.mass = value;
            else if (!strcmp(a->name, "income"))
                dispatchJob.income = value;
            else if (!strcmp(a->name, "planned_distance.km"))
                dispatchJob.distanceKm = value;
        }
    }
    dispatchJob.active = !dispatchJob.cargo.empty();
    if (dispatchJob.active)
        dispatchLastActiveJob = dispatchJob;
    dispatchJobSent = 0;
    return true;
}
void dispatchGameplayEvent(const void* info) {
    const auto* event = static_cast<const scs_telemetry_gameplay_event_t*>(info);
    if (!event || !event->id || (strcmp(event->id, "job.delivered") && strcmp(event->id, "job.cancelled")))
        return;
    dispatchGameplay = event->id;
    dispatchFinishedJob = dispatchJob.active ? dispatchJob : dispatchLastActiveJob;
    dispatchEventAt = GetTickCount64();
    dispatchEventId = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(dispatchEventAt);
    dispatchJobSent = 0;
    dispatchRevenue = dispatchXp = dispatchDistance = 0;
    for (auto a = event->attributes; a && a->name; ++a) {
        const double value = dispatchNumber(a->value);
        if (!std::isfinite(value))
            continue;
        if (!strcmp(a->name, "revenue"))
            dispatchRevenue = value;
        else if (!strcmp(a->name, "earned.xp"))
            dispatchXp = value;
        else if (!strcmp(a->name, "distance.km"))
            dispatchDistance = value;
        else if (!strcmp(a->name, "cargo.damage"))
            dispatchFinishedJob.cargoDamage = static_cast<float>(value);
    }
}
void dispatchTransmitPacket(bool finished) {
    const auto& job = finished ? dispatchFinishedJob : dispatchJob;
    char numbers[400]{};
    _snprintf_s_l(numbers, sizeof(numbers), _TRUNCATE,
                  ",\"massKg\":%.3f,\"income\":%.2f,\"distanceKm\":%.3f,\"cargoDamage\":%.5f,\"revenue\":%."
                  "2f,\"xp\":%.0f,\"drivenKm\":%.3f",
                  jsonLocale, job.mass, job.income, job.distanceKm, static_cast<double>(job.cargoDamage),
                  finished ? dispatchRevenue : 0, finished ? dispatchXp : 0, finished ? dispatchDistance : 0);
    const std::string packet =
        std::string("{\"protocol\":1,\"type\":\"dispatch_job\",\"gameId\":\"eut2\",\"job\":{\"supported\":"
                    "true,\"active\":") +
        (finished     ? "false"
         : job.active ? "true"
                      : "false") +
        ",\"cargo\":" + jsonQuote(job.cargo) + ",\"source\":" +
        jsonQuote(job.sourceCompany.empty() ? "" : job.sourceCompany + "." + job.sourceCity) +
        ",\"destination\":" +
        jsonQuote(job.destinationCompany.empty() ? "" : job.destinationCompany + "." + job.destinationCity) +
        numbers + ",\"liveCreation\":" + (dispatchLiveSupported ? "true" : "false") +
        ",\"event\":" + (finished ? jsonQuote(dispatchGameplay) : "null") +
        ",\"eventId\":" + (finished ? jsonQuote(dispatchEventId) : "null") + "}}";
    sendto(output, packet.c_str(), static_cast<int>(packet.size()), 0,
           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
}
void dispatchTransmit() {
    if (output == INVALID_SOCKET || !sessionActive)
        return;
    const auto now = GetTickCount64();
    if (dispatchJobSent && now - dispatchJobSent < 1000)
        return;
    dispatchJobSent = now;
    // Repeat a terminal event across a short UDP-loss window, but always follow it
    // with CURRENT state so accepting another job is never hidden by old history.
    if (!dispatchGameplay.empty() && now - dispatchEventAt < 20000)
        dispatchTransmitPacket(true);
    dispatchTransmitPacket(false);
}
