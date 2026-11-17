// Shared server messages rendered by the stock ETS2 HUD.
struct NativeNotice {
    std::string text;
    bool official;
    ULONGLONG queued;
};
SRWLOCK nativeNoticeLock = SRWLOCK_INIT;
std::vector<NativeNotice> nativeNotices;
std::atomic<bool> nativeNoticeReady{false};
Ptr nativeHudEngine = 0;
using NoticeTick = void (*)(Ptr);
NoticeTick originalNoticeTick = nullptr;

bool enqueueNativeNotice(const wchar_t* text, bool official) {
    if (!nativeNoticeReady || !text)
        return false;
    const auto length = wcsnlen_s(text, 281);
    if (!length || length > 280)
        return false;
    char utf8[1121]{};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), utf8,
                                          1120, nullptr, nullptr);
    if (bytes <= 0)
        return false;
    std::string clean(utf8, bytes);
    // Native HUD strings support markup: never interpret an announcement as UI.
    for (auto& c : clean)
        if (c == '<' || c == '>')
            c = ' ';
    AcquireSRWLockExclusive(&nativeNoticeLock);
    bool okay = nativeNotices.size() < 8;
    if (okay) {
        for (const auto& item : nativeNotices)
            if (item.text == clean) {
                okay = false;
                break;
            }
    }
    if (okay)
        nativeNotices.push_back({std::move(clean), official, GetTickCount64()});
    ReleaseSRWLockExclusive(&nativeNoticeLock);
    return okay;
}
bool submitNativeNotice(Ptr manager, const char* text, bool official) {
    __try {
        if (!*reinterpret_cast<unsigned char*>(manager + 0x38) ||
            *reinterpret_cast<unsigned char*>(manager + 0x3a) ||
            !*reinterpret_cast<unsigned char*>(manager + 0x3b))
            return false;
        using Submit = void (*)(Ptr, const char**, unsigned, Ptr, unsigned, unsigned short);
        // 0x622fa0 is the stock transient notification wrapper. Icon 2 = announcement,
        // icon 1 = warning. Empty icon override uses the stock shared empty string.
        reinterpret_cast<Submit>(nativeHudEngine + 0x622fa0)(manager, &text, official ? 2u : 1u,
                                                             nativeHudEngine + 0x27949e8, 2, 0xffff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nativeNoticeReady = false;
        return false;
    }
}
void nativeNoticeTick(Ptr manager) {
    if (nativeNoticeReady) {
        NativeNotice item{};
        AcquireSRWLockExclusive(&nativeNoticeLock);
        const auto now = GetTickCount64();
        while (!nativeNotices.empty() && now - nativeNotices.front().queued > 15000)
            nativeNotices.erase(nativeNotices.begin());
        if (!nativeNotices.empty()) {
            item = std::move(nativeNotices.front());
            nativeNotices.erase(nativeNotices.begin());
        }
        ReleaseSRWLockExclusive(&nativeNoticeLock);
        if (!item.text.empty() && !submitNativeNotice(manager, item.text.c_str(), item.official) &&
            nativeNoticeReady) {
            AcquireSRWLockExclusive(&nativeNoticeLock);
            if (nativeNotices.size() < 8)
                nativeNotices.insert(nativeNotices.begin(), std::move(item));
            ReleaseSRWLockExclusive(&nativeNoticeLock);
        }
    }
    originalNoticeTick(manager);
}
bool installNativeHud(Ptr engine) {
    if (nativeNoticeReady)
        return true;
    const unsigned char tickBytes[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x80,
                                       0x79, 0x38, 0x00, 0x48, 0x8b, 0xf9};
    const unsigned char submitBytes[] = {0x4c, 0x8b, 0xdc, 0x48, 0x83, 0xec, 0x68, 0x49,
                                         0x8d, 0x43, 0x38, 0x41, 0xc6, 0x43, 0x38, 0x00};
    auto tick = reinterpret_cast<void*>(engine + 0x622b90);
    if (memcmp(tick, tickBytes, sizeof(tickBytes)) ||
        memcmp(reinterpret_cast<void*>(engine + 0x622fa0), submitBytes, sizeof(submitBytes)))
        return false;
    // The engine calls this hook until process exit, including SDK restarts.
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&nativeNoticeTick), &retained))
        return false;
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (MH_CreateHook(tick, reinterpret_cast<void*>(&nativeNoticeTick),
                      reinterpret_cast<void**>(&originalNoticeTick)) != MH_OK)
        return false;
    nativeHudEngine = engine;
    if (MH_EnableHook(tick) != MH_OK) {
        MH_RemoveHook(tick);
        return false;
    }
    nativeNoticeReady = true;
    return true;
}
