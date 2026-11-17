#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <locale.h>
#include "scssdk_telemetry.h"
#pragma comment(lib, "ws2_32.lib")
SOCKET output = INVALID_SOCKET;
sockaddr_in destination{};
bool sessionActive = false;
_locale_t jsonLocale = nullptr;
std::string jsonQuote(const std::string& value) {
    return "\"" + value + "\"";
}
#include "heavymp_dispatch_telemetry.inl"
int main() {
    scs_named_value_t values[7]{};
    const char* names[] = {"cargo.id", "source.company.id", "source.city.id", "destination.company.id",
                           "destination.city.id"};
    const char* strings[] = {"pears", "euroacres", "erfurt", "sellplan", "lodz"};
    for (unsigned i = 0; i < 5; i++) {
        values[i].name = names[i];
        values[i].value.type = SCS_VALUE_TYPE_string;
        values[i].value.value_string.value = strings[i];
    }
    values[5].name = "cargo.mass";
    values[5].value.type = SCS_VALUE_TYPE_float;
    values[5].value.value_float.value = 12345;
    scs_telemetry_configuration_t config{};
    config.id = "job";
    config.attributes = values;
    if (!dispatchConfig(&config) || !dispatchJob.active || dispatchJob.cargo != "pears" ||
        dispatchJob.mass != 12345)
        return 1;
    scs_named_value_t empty[1]{};
    config.attributes = empty;
    dispatchConfig(&config);
    if (dispatchJob.active)
        return 2;
    scs_named_value_t result[3]{};
    result[0].name = "revenue";
    result[0].value.type = SCS_VALUE_TYPE_s64;
    result[0].value.value_s64.value = 12500;
    result[1].name = "earned.xp";
    result[1].value.type = SCS_VALUE_TYPE_u32;
    result[1].value.value_u32.value = 850;
    scs_telemetry_gameplay_event_t event{};
    event.id = "job.delivered";
    event.attributes = result;
    dispatchGameplayEvent(&event);
    if (dispatchFinishedJob.cargo != "pears" || dispatchFinishedJob.sourceCompany != "euroacres" ||
        dispatchRevenue != 12500 || dispatchXp != 850 || dispatchEventId.empty())
        return 3;
    event.id = "player.fined";
    dispatchGameplayEvent(&event);
    if (dispatchGameplay != "job.delivered")
        return 4;
    config.id = "truck";
    if (dispatchConfig(&config))
        return 5;
    puts("PASS: SDK job config; terminal event survives cleared config; revenue/XP types; unrelated events "
         "ignored.");
    return 0;
}
