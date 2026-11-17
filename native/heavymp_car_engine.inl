// Own the sedan loop's playing state: the truck sound controller must not gate
// it according to throttle. Its loop has an audible idle at revs=.15/load=0.
Ptr localCarActor = 0, localCarEngineEvent = 0;
std::string localCarReference;
ULONGLONG localCarCreated = 0;
ULONGLONG localCarDiagnosticAt = 0;
ULONGLONG localCarGainAt = 0;
float localCarGain = 1.0f;
bool localCarFault = false, localCarLogged = false;
HeavyMpCarEngineState localCarEngineState;
CarEngineEffect localCarEffect;
void freeLocalCarAudio() {
    restoreSedanOriginals();
    releaseCarSound(localCarEngineEvent);
    freeCarEffect(localCarEffect);
    localCarActor = 0;
    localCarReference.clear();
    localCarCreated = 0;
    localCarDiagnosticAt = localCarGainAt = 0;
    localCarGain = 1;
    localCarLogged = false;
    localCarEngineState.reset();
}
void guardedFreeLocalCarAudio() {
    __try {
        freeLocalCarAudio();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        localCarActor = localCarEngineEvent = 0;
        localCarEffect = {};
        localCarEngineState.reset();
    }
}
void localCarEngineFrame(const scs_value_dplacement_t& sdk, float rpm, float throttle, float speed,
                         bool running, bool driving) {
    if (!enabled || faulted || !engineAudioReady || localCarFault || !driving) {
        freeLocalCarAudio();
        return;
    }
    if (!ownerThread || GetCurrentThreadId() != ownerThread)
        return;
    std::array<std::string, 3> refs{};
    if (!readLocalAudio(&refs) || !isLightVehicleEngine(refs[0])) {
        freeLocalCarAudio();
        return;
    }
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8))
        return;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x1d48))
        return;
    if (localCarActor != actor || localCarReference != refs[0]) {
        freeLocalCarAudio();
        localCarActor = actor;
        localCarReference = refs[0];
    }
    HeavyMpNativePeer peer;
    peer.x = sdk.position.x;
    peer.y = sdk.position.y;
    peer.z = sdk.position.z;
    peer.heading = sdk.orientation.heading;
    peer.pitch = sdk.orientation.pitch;
    peer.roll = sdk.orientation.roll;
    const auto pose = placement(peer);
    const double heading = peer.heading * 6.283185307179586;
    const std::array<float, 4> velocity{static_cast<float>(-sin(heading) * speed), 0,
                                        static_cast<float>(-cos(heading) * speed), 0};
    const auto edge = localCarEngineState.sample(running, rpm);
    updateCarEffect(localCarEffect, edge, pose, velocity);
    if (edge == HeavyMpCarEngineState::start)
        log("[HeavyMP audio] Sedan starter: engine transition, one shot.");
    if (!localCarEngineEvent && running) {
        // Match the player's engine category (2), not external ambient vehicles (5).
        const char* path = localCarReference.c_str();
        call<unsigned char>(0x6ae2f0, &localCarEngineEvent, &path, unsigned{2}, unsigned{0x10});
        localCarCreated = GetTickCount64();
    }
    if (!localCarEngineEvent)
        return;
    if (!readable(localCarEngineEvent, 16) || !field<Ptr>(localCarEngineEvent, 8)) {
        if (GetTickCount64() - localCarCreated > 3000) {
            releaseCarSound(localCarEngineEvent);
            log("[HeavyMP audio] Sedan engine resource pending; retrying.");
        }
        return;
    }
    const std::uint64_t tokens[] = {carRevsToken, 0xbfaa8, 0x1ca569d};
    const float values[] = {carRevs(rpm), std::clamp(throttle, 0.0f, 1.0f), std::abs(speed) * 3.6f};
    for (unsigned j = 0; j < 3; ++j) {
        const Ptr p = call<Ptr>(0x6a9ce0, localCarEngineEvent, &tokens[j]);
        if (p)
            call<void>(0x6a9d50, localCarEngineEvent, p, values[j]);
    }
    call<void>(0xae2570, localCarEngineEvent, static_cast<char>(running), &pose, velocity.data());
    if (!sedanFmod.load())
        return;
    const auto instance = sedanFmodInstance(localCarEngineEvent);
    if (!instance)
        return;
    int playback = -1;
    if (sedanFmod.getState(instance, &playback) != 0)
        return;
    // The traffic sedan recording is quiet in the player's cabin (measured
    // audibility 0.09). Lift idle by 12 dB, tapering to 6 dB above 2400 rpm.
    // Smooth the gain so a gear change cannot produce a sharp volume step.
    const float activity = std::isfinite(rpm) ? std::clamp((rpm - 800.0f) / 1600.0f, 0.0f, 1.0f) : 0;
    const float targetGain = 4.0f - 2.0f * activity;
    const auto gainNow = GetTickCount64();
    const float dt = std::clamp(static_cast<float>(gainNow - localCarGainAt) * .001f, 0.0f, .25f);
    localCarGain =
        localCarGainAt ? localCarGain + (targetGain - localCarGain) * (dt / (.15f + dt)) : targetGain;
    localCarGainAt = gainNow;
    sedanFmod.setVolume(instance, localCarGain);
    if (running) {
        sedanFmod.setPaused(instance, 0);
        // STOPPED=2. The native wrapper's queued flag can remain 1 after FMOD stops.
        if (playback == 2 && sedanFmod.start(instance) != 0) {
            restoreSedanOriginals();
            return;
        }
    }
    // Mute only the original sedan engine wrappers, once the replacement loaded.
    // Other engine types, horns and the independent siren remain untouched.
    const Ptr audio = field<Ptr>(actor, 0x1d40);
    if (readable(audio, 0x60))
        for (unsigned offset : {8u, 0x38u, 0x58u}) {
            const Ptr event = field<Ptr>(audio, offset);
            if (!readable(event, 16) || field<Ptr>(event, 0) != engine + 0x22e1280)
                continue;
            const Ptr backend = field<Ptr>(event, 8);
            if (!readable(backend, 0x50))
                continue;
            std::string ref;
            if (readString(backend + 0x38, ref, 240) && ref == localCarReference)
                muteSedanOriginal(event);
        }
    if (GetTickCount64() - localCarDiagnosticAt >= 10000 || !localCarLogged) {
        localCarDiagnosticAt = GetTickCount64();
        localCarLogged = true;
        float volume = -1, finalVolume = -1, revs = -1, load = -1, ignored = 0, audibility = -1;
        int virtualized = -1;
        sedanFmod.getVolume(instance, &volume, &finalVolume);
        sedanFmod.getParameter(instance, "revs", &revs, &ignored);
        sedanFmod.getParameter(instance, "load", &load, &ignored);
        if (sedanFmod.isVirtual)
            sedanFmod.isVirtual(instance, &virtualized);
        void* group = nullptr;
        if (sedanFmod.getGroup && sedanFmod.getAudibility && sedanFmod.getGroup(instance, &group) == 0 &&
            group)
            sedanFmod.getAudibility(group, &audibility);
        char line[400]{};
        sprintf_s(line,
                  "[HeavyMP audio v2] engine=%u rpm=%.0f speed=%.2f FMODstate=%d revs=%.3f load=%.3f "
                  "volume=%.3f final=%.3f virtual=%d audibility=%.5f originalsMuted=%u",
                  running ? 1u : 0u, rpm, speed, playback, revs, load, volume, finalVolume, virtualized,
                  audibility,
                  static_cast<unsigned>(std::count_if(sedanMutedOriginals.begin(), sedanMutedOriginals.end(),
                                                      [](const auto& s) { return s.instance != nullptr; })));
        log(line);
    }
}
void guardedCarEngineFrame(const scs_value_dplacement_t& p, float rpm, float throttle, float speed,
                           bool running, bool driving) {
    __try {
        localCarEngineFrame(p, rpm, throttle, speed, running, driving);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        guardedFreeLocalCarAudio();
        localCarFault = true;
        log("[HeavyMP audio] Sedan audio fault isolated; vehicle and siren preserved.");
    }
}
