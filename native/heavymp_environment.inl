// Internal APIs verified against the executable fingerprint in heavymp_native.cpp.
// The sky evaluator temporarily borrows the room time and restores the exact
// career values with __finally. Ordinary economy readers retain local time.
struct SharedWorld {
    double minutes = 0, scale = 19;
    float blend = 0;
    unsigned weather = 0;
    ULONGLONG at = 0;
    bool active = false;
};
SRWLOCK worldLock = SRWLOCK_INIT;
SharedWorld sharedWorld{};
std::atomic<Ptr> worldEconomy{0};
bool environmentReady = false;
std::atomic<bool> environmentFault{false};
using WorldTime = unsigned (*)(Ptr);
using DisplayTime = unsigned (*)(unsigned);
using WeatherSet = void (*)(Ptr, int, char);
using WeatherProgress = float (*)(Ptr);
using SkyUpdate = void (*)(Ptr);
WorldTime originalWorldTime = nullptr;
DisplayTime originalDisplayTime = nullptr;
WeatherSet originalWeatherSet = nullptr;
WeatherProgress originalWeatherProgress = nullptr;
SkyUpdate originalSkyUpdate = nullptr;
void* environmentHooks[5]{};
thread_local bool settingWeather = false;
thread_local bool calculatingRoomSky = false;
thread_local unsigned skyCareerMinutes = 0;
Ptr savedEconomy = 0;
int savedWeather = 0, savedTarget = 0;
SharedWorld worldState() {
    AcquireSRWLockShared(&worldLock);
    auto w = sharedWorld;
    ReleaseSRWLockShared(&worldLock);
    if (GetTickCount64() - w.at > 1500)
        w.active = false;
    return w;
}
bool clockReader(Ptr caller) {
    switch (caller) {
    case 0x54b76d:
    case 0x87909e:
    case 0xf153a4:
    case 0x1130237:
    case 0x1239c65:
    case 0x1239cc6:
    case 0x13a12e6:
        return true;
    default:
        return false;
    }
}
unsigned roomMinutes(unsigned local, Ptr caller, const SharedWorld& w) {
    if (!w.active || !clockReader(caller))
        return local;
    return static_cast<unsigned>(
        floor(w.minutes + static_cast<double>(GetTickCount64() - w.at) * w.scale / 60000.0));
}
unsigned worldTimeHook(Ptr world) {
    const auto native = originalWorldTime(world);
    const auto local = calculatingRoomSky ? skyCareerMinutes : native;
    const auto w = worldState();
    return environmentFault || !worldEconomy
               ? local
               : roomMinutes(local, reinterpret_cast<Ptr>(_ReturnAddress()) - engine, w);
}
extern "C" unsigned HeavyMpDisplayTimeThunk(unsigned minutes);
extern "C" unsigned HeavyMpDisplayTimeBridge(unsigned minutes, Ptr caller) {
    const auto local = originalDisplayTime(minutes);
    const auto w = worldState();
    return environmentFault || !worldEconomy ? local : roomMinutes(local, caller - engine, w);
}
void weatherSetHook(Ptr economy, int target, char instant) {
    if (!settingWeather && !environmentFault && economy == worldEconomy && worldState().active)
        return;
    originalWeatherSet(economy, target, instant);
}
float weatherProgressHook(Ptr economy) {
    const auto w = worldState();
    if (!environmentFault && w.active && economy == worldEconomy && readable(economy, 0x4584) &&
        field<int>(economy, 0x457c) == 1 && field<int>(economy, 0x3e78) != field<int>(economy, 0x3e7c))
        return field<int>(economy, 0x3e7c) == 1 ? w.blend : 1 - w.blend;
    return originalWeatherProgress(economy);
}
void calculateRoomSky(Ptr economy, const SharedWorld& w) {
    const unsigned minutes = field<unsigned>(economy, 0x3e80), transition = field<unsigned>(economy, 0x4580);
    const float period = field<float>(economy, 0x3e84);
    const double time = w.minutes + static_cast<double>(GetTickCount64() - w.at) * w.scale / 60000.0;
    const unsigned room = static_cast<unsigned>(floor(time));
    const float fraction = static_cast<float>(time - floor(time));
    const bool previous = calculatingRoomSky;
    const unsigned previousCareer = skyCareerMinutes;
    __try {
        calculatingRoomSky = true;
        skyCareerMinutes = previous ? previousCareer : minutes;
        field<unsigned>(economy, 0x3e80) = room;
        field<float>(economy, 0x3e84) = fraction;
        if (field<int>(economy, 0x457c) == 1) {
            const double offset =
                (static_cast<double>(room % 1440) - minutes % 1440 + fraction - period) * 60000;
            const double remapped = fmod(transition + offset, 86400000.0);
            field<unsigned>(economy, 0x4580) =
                static_cast<unsigned>(remapped < 0 ? remapped + 86400000 : remapped);
        }
        originalSkyUpdate(economy);
    } __finally {
        field<unsigned>(economy, 0x3e80) = minutes;
        field<float>(economy, 0x3e84) = period;
        field<unsigned>(economy, 0x4580) = transition;
        calculatingRoomSky = previous;
        skyCareerMinutes = previousCareer;
    }
}
void skyUpdateHook(Ptr economy) {
    const auto w = worldState();
    if (environmentFault || !w.active || economy != worldEconomy || !readable(economy, 0x4588) ||
        !std::isfinite(field<float>(economy, 0x3e84))) {
        originalSkyUpdate(economy);
        return;
    }
    __try {
        calculateRoomSky(economy, w);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        environmentFault = true;
        log("[HeavyMP world] Sky update fault; exact career clock restored, vehicle bridge preserved.");
    }
}
void setRoomWeather(Ptr economy, int target, char instant) {
    settingWeather = true;
    originalWeatherSet(economy, target, instant);
    settingWeather = false;
}
void environmentTick() {
    if (!environmentReady || environmentFault)
        return;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    Ptr economy = 0;
    if (readable(world, 0x31b8) && field<Ptr>(world, 0x31b0))
        economy = field<Ptr>(world, 0x998);
    if (!readable(economy, 0x4738) || !readable(field<Ptr>(economy, 0x2ab8), 160) ||
        !readable(field<Ptr>(economy, 0x2ae0), 0x20))
        economy = 0;
    worldEconomy = economy;
    const auto w = worldState();
    if (!w.active || !economy) {
        if (savedEconomy && savedEconomy == economy) {
            setRoomWeather(economy, savedWeather, 1);
            if (savedTarget != savedWeather)
                setRoomWeather(economy, savedTarget, 0);
        }
        savedEconomy = 0;
        return;
    }
    if (savedEconomy != economy) {
        savedEconomy = economy;
        savedWeather = field<int>(economy, 0x3e78);
        savedTarget = field<int>(economy, 0x3e7c);
        log("[HeavyMP world] Shared display/sky clock and native weather active; career time preserved.");
    }
    if (w.blend <= 0 || w.blend >= 1) {
        const int target = w.blend >= 1 ? 1 : 0;
        if (field<int>(economy, 0x3e78) != target || field<int>(economy, 0x3e7c) != target ||
            field<int>(economy, 0x457c) == 1)
            setRoomWeather(economy, target, 1);
    } else if (field<int>(economy, 0x3e78) != 0 || field<int>(economy, 0x3e7c) != 1 ||
               field<int>(economy, 0x457c) != 1) {
        field<int>(economy, 0x3e78) = 0;
        field<int>(economy, 0x457c) = 0;
        for (size_t p = 0x4568; p <= 0x4574; p += 4)
            field<int>(economy, p) = -1;
        setRoomWeather(economy, 1, 0);
    }
    field<float>(economy, 0x45e8) = w.blend;
    // Stable common sky variant and no independently randomized thunderstorm.
    field<unsigned>(economy, 0x4578) = 0x484d5000;
    field<unsigned char>(economy, 0x3f19) = 0;
}
void guardedEnvironmentTick() {
    __try {
        environmentTick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        settingWeather = false;
        environmentFault = true;
        worldEconomy = 0;
        log("[HeavyMP world] Environment call fault; environment sync disabled, vehicle bridge preserved.");
    }
}
bool attachEnvironment() {
    constexpr Ptr rvas[] = {0x4e74d0, 0x53af70, 0x4fc280, 0x4ed7b0, 0x4f12e0};
    constexpr unsigned char entries[][8] = {{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b},
                                            {0x44, 0x8b, 0xc1, 0x48, 0x8b, 0x0d, 0xc6, 0xa0},
                                            {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x7c},
                                            {0x44, 0x8b, 0x81, 0x80, 0x3e, 0x00, 0x00, 0xb8},
                                            {0x4c, 0x8b, 0xdc, 0x57, 0x48, 0x81, 0xec, 0x40}};
    void* hooks[] = {reinterpret_cast<void*>(&worldTimeHook),
                     reinterpret_cast<void*>(&HeavyMpDisplayTimeThunk),
                     reinterpret_cast<void*>(&weatherSetHook), reinterpret_cast<void*>(&weatherProgressHook),
                     reinterpret_cast<void*>(&skyUpdateHook)};
    void** originals[] = {
        reinterpret_cast<void**>(&originalWorldTime), reinterpret_cast<void**>(&originalDisplayTime),
        reinterpret_cast<void**>(&originalWeatherSet), reinterpret_cast<void**>(&originalWeatherProgress),
        reinterpret_cast<void**>(&originalSkyUpdate)};
    for (unsigned i = 0; i < 5; ++i)
        if (memcmp(reinterpret_cast<void*>(engine + rvas[i]), entries[i], 8))
            return false;
    bool okay = true;
    for (unsigned i = 0; i < 5 && okay; ++i) {
        environmentHooks[i] = reinterpret_cast<void*>(engine + rvas[i]);
        okay = MH_CreateHook(environmentHooks[i], hooks[i], originals[i]) == MH_OK &&
               MH_EnableHook(environmentHooks[i]) == MH_OK;
    }
    if (!okay)
        for (auto& h : environmentHooks)
            if (h) {
                MH_DisableHook(h);
                MH_RemoveHook(h);
                h = nullptr;
            }
    return okay;
}
void receiveWorld(const unsigned char* p, size_t n) {
    if (n != 40 || memcmp(p, "HMW1", 4))
        return;
    unsigned active = 0, reserved = 0;
    memcpy(&active, p + 8, 4);
    memcpy(&reserved, p + 12, 4);
    if (active > 1 || reserved)
        return;
    SharedWorld w{};
    w.active = active != 0;
    w.at = GetTickCount64();
    memcpy(&w.minutes, p + 16, 8);
    memcpy(&w.scale, p + 24, 8);
    memcpy(&w.blend, p + 32, 4);
    memcpy(&w.weather, p + 36, 4);
    if (w.active && (!std::isfinite(w.minutes) || w.minutes < 0 || w.minutes > 1e9 ||
                     !std::isfinite(w.scale) || w.scale < 1 || w.scale > 60 || !std::isfinite(w.blend) ||
                     w.blend < 0 || w.blend > 1 || w.weather > 1))
        return;
    AcquireSRWLockExclusive(&worldLock);
    sharedWorld = w;
    ReleaseSRWLockExclusive(&worldLock);
}
