// Cooperative loading: the game's SDK plugin loads the UI into its own process.
// The request contains no arbitrary DLL path and expires within one minute.
HANDLE sessionRequestStop = nullptr, sessionRequestWorker = nullptr;
volatile LONG sessionRequestResult = 0;
ULONGLONG sessionPoll = 0;
std::array<unsigned char, 16> processedRequest{};
ULONGLONG utcMilliseconds() {
    FILETIME t{};
    GetSystemTimeAsFileTime(&t);
    return ((static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000 - 11644473600000ULL;
}
ULONGLONG processBirth(HANDLE p) {
    FILETIME a{}, b{}, c{}, d{};
    if (!GetProcessTimes(p, &a, &b, &c, &d))
        return 0;
    return ((static_cast<ULONGLONG>(a.dwHighDateTime) << 32) | a.dwLowDateTime) / 10000 - 11644473600000ULL;
}
void pollLauncherUi() {
    const auto tick = GetTickCount64();
    if (sessionPoll && tick - sessionPoll < 200)
        return;
    sessionPoll = tick;
    wchar_t directory[32768]{};
    const auto n = GetEnvironmentVariableW(L"APPDATA", directory, 32768);
    if (!n || n >= 32768)
        return;
    const std::wstring root = std::wstring(directory) + L"\\heavymp-launcher\\";
    HANDLE file = CreateFileW((root + L"launch-request.bin").c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    std::array<unsigned char, 48> data{};
    DWORD read = 0;
    LARGE_INTEGER length{};
    const bool valid = GetFileSizeEx(file, &length) && length.QuadPart == 48 &&
                       ReadFile(file, data.data(), 48, &read, nullptr) && read == 48;
    CloseHandle(file);
    if (!valid || memcmp(data.data(), "HMUI", 4))
        return;
    unsigned version = 0, pid = 0, launcherPid = 0;
    ULONGLONG birth = 0, expires = 0;
    memcpy(&version, data.data() + 4, 4);
    memcpy(&pid, data.data() + 8, 4);
    memcpy(&launcherPid, data.data() + 12, 4);
    memcpy(&birth, data.data() + 16, 8);
    memcpy(&expires, data.data() + 24, 8);
    const auto utc = utcMilliseconds();
    if (version != 1 || pid != GetCurrentProcessId() || birth != processBirth(GetCurrentProcess()) ||
        expires < utc || expires - utc > 60000 || !memcmp(processedRequest.data(), data.data() + 32, 16))
        return;
    HANDLE launcher = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, launcherPid);
    if (!launcher)
        return;
    wchar_t executable[32768]{};
    DWORD size = 32768;
    const bool found = QueryFullProcessImageNameW(launcher, 0, executable, &size) != FALSE;
    CloseHandle(launcher);
    if (!found)
        return;
    auto* slash = wcsrchr(executable, L'\\');
    if (!slash || _wcsicmp(slash + 1, L"HeavyMP.exe"))
        return;
    *slash = 0;
    const std::wstring uiPath =
        std::wstring(executable) + L"\\resources\\heavymp\\native\\build\\heavymp-ui.dll";
    DWORD error = 0;
    HMODULE ui = GetModuleHandleW(uiPath.c_str());
    if (!ui)
        ui = LoadLibraryExW(uiPath.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!ui)
        error = GetLastError();
    else {
        using Start = DWORD(WINAPI*)(void*);
        const auto start = reinterpret_cast<Start>(GetProcAddress(ui, "HeavyMpUiStart"));
        if (!start)
            error = ERROR_PROC_NOT_FOUND;
        else if (start(nullptr) != 1)
            error = ERROR_DLL_INIT_FAILED;
    }
    memcpy(data.data(), error ? "HMNO" : "HMOK", 4);
    memcpy(data.data() + 4, &error, 4);
    const std::wstring temporary = root + L"launch-response.tmp", response = root + L"launch-response.bin";
    file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const bool complete = WriteFile(file, data.data(), 48, &written, nullptr) && written == 48;
        CloseHandle(file);
        if (complete && MoveFileExW(temporary.c_str(), response.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            memcpy(processedRequest.data(), data.data() + 32, 16);
        else
            DeleteFileW(temporary.c_str());
    }
    InterlockedExchange(&sessionRequestResult, error ? 2 : 1);
}

// Only file exchange and UI loading run here. Engine/SDK calls stay on SDK callbacks.
DWORD WINAPI sessionRequestLoop(void*) {
    while (WaitForSingleObject(sessionRequestStop, 200) == WAIT_TIMEOUT)
        pollLauncherUi();
    return 0;
}
void stopSessionRequests() {
    if (sessionRequestStop)
        SetEvent(sessionRequestStop);
    if (sessionRequestWorker) {
        WaitForSingleObject(sessionRequestWorker, INFINITE);
        CloseHandle(sessionRequestWorker);
        sessionRequestWorker = nullptr;
    }
    if (sessionRequestStop) {
        CloseHandle(sessionRequestStop);
        sessionRequestStop = nullptr;
    }
}
bool startSessionRequests() {
    stopSessionRequests();
    sessionPoll = 0;
    processedRequest = {};
    InterlockedExchange(&sessionRequestResult, 0);
    sessionRequestStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (sessionRequestStop)
        sessionRequestWorker = CreateThread(nullptr, 0, sessionRequestLoop, nullptr, 0, nullptr);
    if (!sessionRequestWorker) {
        stopSessionRequests();
        return false;
    }
    return true;
}
void logLauncherUiResult() {
    const auto result = InterlockedExchange(&sessionRequestResult, 0);
    if (result && sessionApi.common.log)
        sessionApi.common.log(
            result == 2 ? SCS_LOG_TYPE_error : SCS_LOG_TYPE_message,
            result == 2
                ? "[HeavyMP session] Cooperative UI loading failed; check launcher error code."
                : "[HeavyMP session] UI loaded by request listener, independent of telemetry frames.");
}
