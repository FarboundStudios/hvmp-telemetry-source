bool engineAudioReady = false, engineAudioFault = false;
// Prism tokens use base 38, least significant character first.
constexpr std::uint64_t carRevsToken = 0x18fece;
constexpr const char* carEngineReference = "/def/vehicle/truck/heavymp.megane/engine/sound_engine.soundref";
constexpr const char* escortEngineReference =
    "/def/vehicle/truck/heavymp.escort/engine/sound_engine.soundref";
constexpr const char* ambulanceEngineReference =
    "/def/vehicle/truck/heavymp.ambulance/engine/sound_engine.soundref";
bool isLightVehicleEngine(const std::string& reference) {
    return !heavyMpLightFamily(reference).empty();
}
// The sedan loop's authored idle is 0.15; retain full range at the 4800 rpm
// limiter. Use the same mapping locally and for nearby multiplayer vehicles.
float carRevs(float rpm) {
    return std::isfinite(rpm) ? std::clamp(.15f + (rpm - 800.0f) * (.85f / 4000.0f), 0.0f, 1.0f) : 0.0f;
}
void releaseCarSound(Ptr& event) {
    if (event && readable(event, 16) && field<Ptr>(event, 0) == engine + 0x22e1280) {
        method<void>(event, 0x38, unsigned{1});
        method<void>(event, 0, unsigned{1});
    }
    event = 0;
}
void freeCarEffect(CarEngineEffect& effect) {
    releaseCarSound(effect.event);
    effect = {};
}
void updateCarEffect(CarEngineEffect& effect, HeavyMpCarEngineState::Edge edge, const Placement& pose,
                     const std::array<float, 4>& velocity) {
    if (edge != HeavyMpCarEngineState::none) {
        freeCarEffect(effect);
        const char* path = edge == HeavyMpCarEngineState::start
                               ? "/def/vehicle/truck/heavymp.megane/engine/sound_start.soundref"
                               : "/def/vehicle/truck/heavymp.megane/engine/sound_stop.soundref";
        call<unsigned char>(0x6ae2f0, &effect.event, &path, unsigned{5}, unsigned{0x10});
        effect.created = GetTickCount64();
    }
    if (!effect.event)
        return;
    if (GetTickCount64() - effect.created > 10000) {
        freeCarEffect(effect);
        return;
    }
    // Play a start/stop recording once, after its asynchronous resource is ready.
    if (!effect.started && readable(effect.event, 16) && field<Ptr>(effect.event, 8)) {
        call<void>(0xae2570, effect.event, char{1}, &pose, velocity.data());
        effect.started = true;
    }
}
void freeEngineEvent(Truck& t, unsigned i) {
    const Ptr event = t.engineEvents[i];
    if (event && readable(event, 16) && field<Ptr>(event, 0) == engine + 0x22e1280) {
        method<void>(event, 0x38, unsigned{1});
        method<void>(event, 0, unsigned{1});
    }
    t.engineEvents[i] = 0;
    t.engineParameters[i] = {};
}
void freeEngineAudio(Truck& t) {
    for (unsigned i = 0; i < 3; ++i)
        freeEngineEvent(t, i);
    freeCarEffect(t.carEngineEffect);
    t.carEngineState.reset();
}
void guardedFreeEngineAudio(Truck& t) {
    __try {
        freeEngineAudio(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t.engineEvents = {};
        t.engineParameters = {};
        t.carEngineEffect = {};
        t.carEngineState.reset();
        t.engineSoundFault = true;
    }
}
bool readLocalAudio(std::array<std::string, 3>* out) {
    if (!out || !enabled || faulted)
        return false;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8))
        return false;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x1d48))
        return false;
    const Ptr audio = field<Ptr>(actor, 0x1d40);
    if (!readable(audio, 0x60))
        return false;
    // Sound reference strings change with the vehicle/configuration, not its RPM.
    // Share this short-lived snapshot across engine, horn and telemetry reads.
    static thread_local Ptr cachedActor = 0, cachedAudio = 0;
    static thread_local ULONGLONG cachedAt = 0;
    static thread_local std::array<std::string, 3> cachedRefs{};
    const auto now = GetTickCount64();
    if (cachedAt && cachedActor == actor && cachedAudio == audio && now - cachedAt < 250) {
        *out = cachedRefs;
        return !(*out)[0].empty();
    }
    const unsigned offsets[] = {8, 0x38, 0x58};
    out->fill({});
    for (unsigned i = 0; i < 3; ++i) {
        const Ptr event = field<Ptr>(audio, offsets[i]);
        if (!readable(event, 16) || field<Ptr>(event, 0) != engine + 0x22e1280)
            continue;
        const Ptr backend = field<Ptr>(event, 8);
        if (!readable(backend, 0x50))
            continue;
        std::string path;
        if (readString(backend + 0x38, path, 240) && heavympAudioReference(path))
            (*out)[i] = std::move(path);
    }
    cachedActor = actor;
    cachedAudio = audio;
    cachedAt = now;
    cachedRefs = *out;
    return !(*out)[0].empty();
}
bool guardedLocalAudio(std::array<std::string, 3>* out) {
    __try {
        return readLocalAudio(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void updateEngineAudio(Truck& t) {
    if (!engineAudioReady || t.engineSoundFault || t.trailer || !t.attached)
        return;
    const auto& a = t.animation;
    const bool running = (a.controls & 1) != 0;
    const auto now = GetTickCount64();
    const Placement pose = placement(a);
    const float heading = static_cast<float>(a.heading * 6.283185307179586);
    const std::array<float, 4> velocity{static_cast<float>(-sin(heading) * a.speed), 0,
                                        static_cast<float>(-cos(heading) * a.speed), 0};
    if (isLightVehicleEngine(a.audioReferences[0]))
        updateCarEffect(t.carEngineEffect, t.carEngineState.sample(running, a.rpm), pose, velocity);
    // Native 0x6a9ce0 accepts prism token_t, not a C string: rpm/load/brake/turbo/speed/gear.
    const std::uint64_t names[] = {0x85b4, 0xbfaa8, 0x1ef1768, 0x3261958, 0x1ca569d, 0x17b1f7};
    const float values[] = {a.rpm,
                            a.throttle,
                            a.brake,
                            a.throttle,
                            static_cast<float>(std::abs(a.speed) * 3.6),
                            static_cast<float>(a.gear)};
    for (unsigned i = 0; i < 3; ++i) {
        const auto& reference = a.audioReferences[i];
        if (!heavympAudioReference(reference))
            continue;
        if (t.loadedAudioReferences[i] != reference) {
            freeEngineEvent(t, i);
            t.loadedAudioReferences[i] = reference;
            t.audioRetryAt[i] = 0;
            t.audioLogged = false;
        }
        if (reference.empty())
            continue;
        if (!t.engineEvents[i] && running) {
            if (t.audioRetryAt[i] && now - t.audioRetryAt[i] < 3000)
                continue;
            t.audioRetryAt[i] = now;
            const char* path = reference.c_str();
            call<unsigned char>(0x6ae2f0, &t.engineEvents[i], &path, unsigned{5}, unsigned{0x10});
            t.audioCreatedAt[i] = now;
        }
        const Ptr event = t.engineEvents[i];
        if (!event)
            continue;
        // A non-null wrapper does not mean the sound resource has loaded.
        if (!readable(event, 16) || !field<Ptr>(event, 8)) {
            if (now - t.audioCreatedAt[i] >= 3000) {
                char line[330]{};
                sprintf_s(line, "[HeavyMP audio] Sound backend unavailable; retrying original reference: %s",
                          reference.c_str());
                log(line);
                freeEngineEvent(t, i);
                t.audioRetryAt[i] = now;
            }
            continue;
        }
        for (unsigned j = 0; j < 6; ++j) {
            if (!t.engineParameters[i][j])
                t.engineParameters[i][j] = call<Ptr>(0x6a9ce0, event, &names[j]);
            if (t.engineParameters[i][j])
                call<void>(0x6a9d50, event, t.engineParameters[i][j], values[j]);
        }
        if (isLightVehicleEngine(reference)) {
            const Ptr parameter = call<Ptr>(0x6a9ce0, event, &carRevsToken);
            if (parameter) {
                call<void>(0x6a9d50, event, parameter, carRevs(a.rpm));
                if (i == 0 && !t.audioLogged) {
                    t.audioLogged = true;
                    log("[HeavyMP audio] Car diesel backend ready; normalized revs/load/speed.");
                }
            }
        }
        call<void>(0xae2570, event, static_cast<char>(running), &pose, velocity.data());
        if (i == 0 && t.engineParameters[i][0] && !t.audioLogged) {
            t.audioLogged = true;
            char line[350]{};
            sprintf_s(line, "[HeavyMP audio] Original engine backend ready; rpm/load=%u/%u reference=%s",
                      t.engineParameters[i][0] ? 1u : 0u, t.engineParameters[i][1] ? 1u : 0u,
                      reference.c_str());
            log(line);
        }
    }
}
void guardedEngineAudio(Truck& t) {
    __try {
        updateEngineAudio(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        guardedFreeEngineAudio(t);
        t.engineSoundFault = true;
        log("[HeavyMP audio] Engine audio fault isolated to this vehicle; renderer preserved.");
    }
}
bool validateEngineAudio() {
    return validateHorns() && field<std::uint64_t>(engine, 0x6a9ce0) == 0x8418b4c28ec8348ULL &&
           field<std::uint64_t>(engine, 0x6a9d50) == 0x8418b4c28ec8348ULL;
}
