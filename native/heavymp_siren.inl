HeavyMpSirenLatch localSirenLatch;
Ptr localSirenEvent = 0;
bool localSirenControlled = false, localSirenFault = false;
Ptr localHornFallbackEvent = 0;
ULONGLONG localSirenCreated = 0;
std::string localSirenFamily;
void freeLocalSiren() {
    for (const Ptr event : {localSirenEvent, localHornFallbackEvent})
        if (event && readable(event, 16) && field<Ptr>(event, 0) == engine + 0x22e1280) {
            method<void>(event, 0x38, unsigned{1});
            method<void>(event, 0, unsigned{1});
        }
    localSirenEvent = localHornFallbackEvent = 0;
    localSirenFamily.clear();
    localSirenLatch.reset();
    localSirenControlled = false;
}
void guardedFreeLocalSiren() {
    __try {
        freeLocalSiren();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        localSirenEvent = localHornFallbackEvent = 0;
        localSirenFamily.clear();
        localSirenLatch.reset();
        localSirenControlled = false;
    }
}
void localSirenFrame(const scs_value_dplacement_t& sdk, float speed, bool driving) {
    if (!enabled || faulted || !hornReady || localSirenFault || !driving) {
        freeLocalSiren();
        return;
    }
    if (!ownerThread || ownerThread != GetCurrentThreadId())
        return;
    std::array<std::string, 3> refs{};
    if (!readLocalAudio(&refs) || !isLightVehicleEngine(refs[0])) {
        freeLocalSiren();
        return;
    }
    const auto family = heavyMpLightFamily(refs[0]);
    const bool police = heavyMpSirenReference(family) != nullptr;
    if (!localSirenFamily.empty() && localSirenFamily != family)
        freeLocalSiren();
    localSirenFamily = family;
    if (!police && localSirenEvent)
        freeLocalSiren();
    unsigned raw = 0;
    if (!readLocalHorns(&raw)) {
        freeLocalSiren();
        return;
    }
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8)) {
        freeLocalSiren();
        return;
    }
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (localSirenLatch.actor && localSirenLatch.actor != actor)
        freeLocalSiren();
    const bool wasOn = localSirenLatch.on;
    const bool on = localSirenLatch.sample(actor, police, (raw & 2) != 0);
    localSirenControlled = police;
    if (on != wasOn)
        log(on ? "[HeavyMP siren] Toggle ON (synchronized)." : "[HeavyMP siren] Toggle OFF (synchronized).");
    if (on && !localSirenEvent) {
        const char* path = heavyMpSirenReference(family);
        call<unsigned char>(0x6ae2f0, &localSirenEvent, &path, unsigned{5}, unsigned{0x10});
        localSirenCreated = GetTickCount64();
    }
    HeavyMpNativePeer peer;
    peer.x = sdk.position.x;
    peer.y = sdk.position.y;
    peer.z = sdk.position.z;
    peer.heading = sdk.orientation.heading;
    peer.pitch = sdk.orientation.pitch;
    peer.roll = sdk.orientation.roll;
    const auto pose = placement(peer);
    const double h = peer.heading * 6.283185307179586;
    const std::array<float, 4> velocity{static_cast<float>(-sin(h) * speed), 0,
                                        static_cast<float>(-cos(h) * speed), 0};
    // Preserve the game's normal horn when available. Supply the stock car horn
    // only when that optional event is missing, without playing two horns at once.
    const Ptr audio = field<Ptr>(actor, actorAudioOffset);
    const Ptr horn = readable(audio, 0x100) ? field<Ptr>(audio, 0xf8) : 0;
    const bool nativeHorn =
        readable(horn, 16) && field<Ptr>(horn, 0) == engine + 0x22e1280 && field<Ptr>(horn, 8);
    const bool fallbackPressed = (raw & 1) && !nativeHorn;
    if (fallbackPressed && !localHornFallbackEvent) {
        const char* path = police ? "/def/vehicle/truck/heavymp.megane/cabin/horn.soundref"
                                  : "/def/vehicle/truck/heavymp.escort/cabin/horn.soundref";
        call<unsigned char>(0x6ae2f0, &localHornFallbackEvent, &path, unsigned{5}, unsigned{0x10});
        if (localHornFallbackEvent)
            log("[HeavyMP horn] Local car horn fallback created; stock horn event absent.");
    }
    if (localHornFallbackEvent && readable(localHornFallbackEvent, 16) &&
        field<Ptr>(localHornFallbackEvent, 8))
        call<void>(0xae2570, localHornFallbackEvent, static_cast<char>(fallbackPressed), &pose,
                   velocity.data());
    if (localSirenEvent) {
        if (!readable(localSirenEvent, 16) || !field<Ptr>(localSirenEvent, 8)) {
            if (GetTickCount64() - localSirenCreated >= 3000) {
                const auto state = localSirenLatch;
                freeLocalSiren();
                localSirenLatch = state;
                localSirenControlled = true;
                log("[HeavyMP siren] Police sound resource pending; retrying.");
            }
            return;
        }
        call<void>(0xae2570, localSirenEvent, static_cast<char>(on), &pose, velocity.data());
    }
}
void guardedLocalSirenFrame(const scs_value_dplacement_t& sdk, float speed, bool driving) {
    __try {
        localSirenFrame(sdk, speed, driving);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        guardedFreeLocalSiren();
        localSirenFault = true;
        log("[HeavyMP siren] Local audio fault isolated; vehicle bridge preserved.");
    }
}
