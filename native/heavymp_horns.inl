bool hornReady = false, hornFault = false;
constexpr std::size_t actorAudioOffset = 0x1d40;
constexpr std::size_t hornFlagsOffset = 0x668;
void releaseHorn(Ptr& event) {
    if (!event)
        return;
    if (readable(event, 16) && field<Ptr>(event, 0) == engine + 0x22e1280) {
        method<void>(event, 0x38, unsigned{1});
        method<void>(event, 0, unsigned{1});
    }
    event = 0;
}
void freeHorns(Truck& t) {
    for (auto& event : t.hornEvents)
        releaseHorn(event);
}
void guardedFreeHorns(Truck& t) {
    __try {
        freeHorns(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t.hornEvents = {};
        hornFault = true;
        log("[HeavyMP horn] Audio cleanup fault; vehicle bridge preserved.");
    }
}
std::string airHornSound(const Truck& t) {
    if (!readable(t.definition, 0x28))
        return {};
    const auto count = field<size_t>(t.definition, 0x20);
    const Ptr array = field<Ptr>(t.definition, 0x18);
    if (count > 256 || !readable(array, count * 8))
        return {};
    for (size_t i = 0; i < count; ++i) {
        const Ptr accessory = field<Ptr>(array, i * 8);
        std::string path;
        if (!readString(accessory + 0x10, path, 256) || path.find("horn") == std::string::npos)
            continue;
        const Ptr data = call<Ptr>(0x8e2f20, accessory);
        if (!readable(data, 0x298) || field<LONG>(data, 8) >= 0 ||
            method<Ptr>(data, 0x28) != engine + 0x1e59db0)
            continue;
        if (readString(data + 0x278, path, 256) && path.size() > 9 && path.front() == '/' &&
            path.substr(path.size() - 9) == ".soundref" && path.find("..") == std::string::npos)
            return path;
    }
    return {};
}
void updateHorns(Truck& t) {
    if (hornFault) {
        guardedFreeHorns(t);
        return;
    }
    if (!hornReady || !t.attached || !t.animation.horn) {
        freeHorns(t);
        return;
    }
    const auto& peer = t.animation;
    const Placement pose = placement(peer);
    const float heading = static_cast<float>(peer.heading * 3.141592653589793 * 2.0);
    const std::array<float, 4> velocity{static_cast<float>(-sin(heading) * peer.speed), 0,
                                        static_cast<float>(-cos(heading) * peer.speed), 0};
    for (unsigned i = 0; i < 2; ++i) {
        if (!(peer.horn & (1u << i))) {
            releaseHorn(t.hornEvents[i]);
            continue;
        }
        if (!t.hornEvents[i]) {
            std::string path = i ? airHornSound(t) : "";
            std::string family;
            if (t.appearance)
                for (const auto& item : t.appearance->accessories)
                    for (const char* car : {"megane", "ambulance", "escort", "sedan"})
                        if (item.path == std::string("/def/vehicle/truck/heavymp.") + car + "/data.sii")
                            family = car;
            if (!family.empty()) {
                const char* ref = i ? heavyMpSirenReference(family) : heavyMpLightHornReference(family);
                if (!ref)
                    continue; // Civilian vehicles have no siren/air horn.
                path = ref;
            }
            if (path.empty())
                path = i ? "/sound/truck/air_horn/air_horn_04.soundref"
                         : "/def/vehicle/truck/scania.r/cabin/sound_horn.soundref";
            const char* ref = path.c_str();
            call<unsigned char>(0x6ae2f0, &t.hornEvents[i], &ref, unsigned{5}, unsigned{0x10});
            if (t.hornEvents[i])
                log(i ? "[HeavyMP horn] Native positional air horn loaded."
                      : "[HeavyMP horn] Native positional horn loaded.");
        }
        if (t.hornEvents[i])
            call<void>(0xae2570, t.hornEvents[i], char{1}, &pose, velocity.data());
    }
}
void guardedHorns(Truck& t) {
    __try {
        updateHorns(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hornFault = true;
        guardedFreeHorns(t);
        log("[HeavyMP horn] Audio update fault; horn sync disabled, vehicle bridge preserved.");
    }
}
bool readLocalHorns(unsigned* out) {
    *out = 0;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8))
        return false;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x1d48))
        return false;
    const Ptr physics = field<Ptr>(actor, 0x18);
    if (!readable(physics, 0x208) || field<Ptr>(physics, 0) != engine + 0x2332578)
        return false;
    // The 1.62.3.3 input update at 0x95ba00 writes bits 0x100/0x200
    // into this controller even when its optional horn sound is absent.
    const Ptr audio = field<Ptr>(actor, actorAudioOffset);
    if (!readable(audio, hornFlagsOffset + sizeof(unsigned short)))
        return false;
    *out = (field<unsigned short>(audio, hornFlagsOffset) >> 8) & 3;
    return true;
}
bool guardedLocalHorns(unsigned* out) {
    __try {
        return readLocalHorns(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = 0;
        return false;
    }
}
bool validateHorns() {
    constexpr unsigned char create[] = {0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74};
    constexpr unsigned char play[] = {0x48, 0x85, 0xc9, 0x74, 0x7d, 0x48, 0x89, 0x5c};
    constexpr unsigned char controller[] = {0x48, 0x8b, 0x8f, 0x40, 0x1d, 0, 0};
    constexpr unsigned char flags[] = {0x66, 0x89, 0x8f, 0x68, 0x06, 0, 0};
    return !memcmp(reinterpret_cast<void*>(engine + 0x6ae2f0), create, sizeof(create)) &&
           !memcmp(reinterpret_cast<void*>(engine + 0xae2570), play, sizeof(play)) &&
           !memcmp(reinterpret_cast<void*>(engine + 0x876d60), controller, sizeof(controller)) &&
           !memcmp(reinterpret_cast<void*>(engine + 0x95ba70), flags, sizeof(flags));
}
