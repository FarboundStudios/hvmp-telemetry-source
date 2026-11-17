// Use the installed FMOD C API for actual playback/volume state, rather than
// treating a Prism parameter node or queued play flag as proof of audible sound.
struct SedanFmodApi {
    using Volume = int (*)(void*, float);
    using ReadVolume = int (*)(void*, float*, float*);
    using State = int (*)(void*, int*);
    using Play = int (*)(void*);
    using Pause = int (*)(void*, int);
    using Parameter = int (*)(void*, const char*, float*, float*);
    using Group = int (*)(void*, void**);
    using Audibility = int (*)(void*, float*);
    Volume setVolume = nullptr;
    ReadVolume getVolume = nullptr;
    State getState = nullptr, isVirtual = nullptr;
    Play start = nullptr;
    Pause setPaused = nullptr;
    Parameter getParameter = nullptr;
    Group getGroup = nullptr;
    Audibility getAudibility = nullptr;
    bool load() {
        if (setVolume && getVolume && getState && start && setPaused && getParameter)
            return true;
        const auto studio = GetModuleHandleW(L"fmodstudio.dll"), core = GetModuleHandleW(L"fmod.dll");
        if (!studio || !core)
            return false;
        setVolume = reinterpret_cast<Volume>(GetProcAddress(studio, "FMOD_Studio_EventInstance_SetVolume"));
        getVolume =
            reinterpret_cast<ReadVolume>(GetProcAddress(studio, "FMOD_Studio_EventInstance_GetVolume"));
        getState =
            reinterpret_cast<State>(GetProcAddress(studio, "FMOD_Studio_EventInstance_GetPlaybackState"));
        isVirtual = reinterpret_cast<State>(GetProcAddress(studio, "FMOD_Studio_EventInstance_IsVirtual"));
        start = reinterpret_cast<Play>(GetProcAddress(studio, "FMOD_Studio_EventInstance_Start"));
        setPaused = reinterpret_cast<Pause>(GetProcAddress(studio, "FMOD_Studio_EventInstance_SetPaused"));
        getParameter = reinterpret_cast<Parameter>(
            GetProcAddress(studio, "FMOD_Studio_EventInstance_GetParameterByName"));
        getGroup =
            reinterpret_cast<Group>(GetProcAddress(studio, "FMOD_Studio_EventInstance_GetChannelGroup"));
        getAudibility = reinterpret_cast<Audibility>(GetProcAddress(core, "FMOD_ChannelGroup_GetAudibility"));
        return setVolume && getVolume && getState && start && setPaused && getParameter;
    }
} sedanFmod;
void* sedanFmodInstance(Ptr wrapper) {
    if (!wrapper || !readable(wrapper, 16) || field<Ptr>(wrapper, 0) != engine + 0x22e1280)
        return nullptr;
    const Ptr backend = field<Ptr>(wrapper, 8);
    // FMOD backend +0xa0 is verified from the 1.62.3.3 sound backend layout.
    if (!readable(backend, 0xa8) || field<unsigned>(backend, 0x2c) != 2)
        return nullptr;
    return reinterpret_cast<void*>(field<Ptr>(backend, 0xa0));
}
struct SedanMutedOriginal {
    Ptr wrapper = 0;
    void* instance = nullptr;
    float volume = 1;
};
std::array<SedanMutedOriginal, 3> sedanMutedOriginals{};
void restoreSedanOriginals() {
    if (sedanFmod.setVolume)
        for (auto& saved : sedanMutedOriginals) {
            if (saved.instance && sedanFmodInstance(saved.wrapper) == saved.instance)
                sedanFmod.setVolume(saved.instance, saved.volume);
        }
    sedanMutedOriginals = {};
}
void muteSedanOriginal(Ptr wrapper) {
    const auto instance = sedanFmodInstance(wrapper);
    if (!instance)
        return;
    for (auto& saved : sedanMutedOriginals)
        if (saved.instance && sedanFmodInstance(saved.wrapper) != saved.instance)
            saved = {};
    for (auto& saved : sedanMutedOriginals)
        if (saved.instance == instance) {
            sedanFmod.setVolume(instance, 0);
            return;
        }
    for (auto& saved : sedanMutedOriginals)
        if (!saved.instance) {
            float volume = 1, finalVolume = 0;
            if (sedanFmod.getVolume(instance, &volume, &finalVolume) != 0)
                return;
            saved = {wrapper, instance, volume};
            sedanFmod.setVolume(instance, 0);
            return;
        }
}
