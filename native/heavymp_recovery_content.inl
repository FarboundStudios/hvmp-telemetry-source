// Session-only vehicle and trailer fallback. The game owns its menus and profiles.
#include "heavymp_recovery_archive.h"
bool nativeRecoveryMounted = false;
// Validate the mounted VFS bytes before admitting a saved profile. Ownership,
// cargo, garages and save graphs remain managed by ETS2, never rewritten here.
bool verifyNativeRecoveryWith(OpenVirtual open) {
    for (const auto& entry : nativeRecoveryEntries) {
        std::vector<unsigned char> bytes(entry.size);
        ULONGLONG actualSize = 0;
        ReadDiagnostics diagnostic{};
        std::array<unsigned char, 32> actual{};
        if (!readVirtualWith(open, entry.path, bytes.data(), bytes.size(), &actualSize, &diagnostic) ||
            !bytesDigest(bytes, actual) || memcmp(actual.data(), entry.sha256, 32)) {
            if (logger) {
                const auto message =
                    std::string("[HeavyMP recovery] Required client fallback verification FAILED: ") +
                    entry.path;
                logger(SCS_LOG_TYPE_error, message.c_str());
            }
            return false;
        }
    }
    return true;
}
bool mountNativeRecovery(Ptr engine) {
    if (nativeRecoveryMounted)
        return true;
    if (!codeMatches(engine))
        return false;
    wchar_t appdata[32768]{};
    const auto length = GetEnvironmentVariableW(L"APPDATA", appdata, 32768);
    if (!length || length >= 32768)
        return false;
    const auto root = std::wstring(appdata) + L"\\heavymp-launcher";
    if (!plainDirectory(root))
        return false;
    const auto directory = root + L"\\native-ui";
    CreateDirectoryW(directory.c_str(), nullptr);
    if (!plainDirectory(directory))
        return false;
    const auto hash = hex(nativeRecoveryZipHash);
    const auto path = directory + L"\\" + std::wstring(hash.begin(), hash.end()) + L".scs";
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const bool good = WriteFile(file, nativeRecoveryZip, sizeof(nativeRecoveryZip), &written, nullptr) &&
                          written == sizeof(nativeRecoveryZip);
        CloseHandle(file);
        if (!good) {
            DeleteFileW(path.c_str());
            return false;
        }
    } else if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    if (!regular(path))
        return false;
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    std::array<unsigned char, 32> actual{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != sizeof(nativeRecoveryZip) || !digest(file, actual) ||
        memcmp(actual.data(), nativeRecoveryZipHash, 32)) {
        CloseHandle(file);
        return false;
    }
    Ptr device = 0;
    const auto text = utf8(path);
    if (text.empty() || !openArchive(engine, text.c_str(), &device)) {
        CloseHandle(file);
        return false;
    }
    mountedDevices.push_back(device);
    if (!mountArchive(engine, device, 10000)) {
        CloseHandle(file);
        return false;
    }
    contentLocks.push_back(file); // Hold the immutable embedded package until exit.
    // Verify the complete recovery VEHICLE, not an accessory unit, at every
    // location the engine can derive from a surviving saved wheel accessory.
    const bool recovery = verifyNativeRecoveryWith(reinterpret_cast<OpenVirtual>(engine + 0x114a30));
    if (logger)
        logger(recovery ? SCS_LOG_TYPE_message : SCS_LOG_TYPE_error,
               recovery ? "[HeavyMP recovery] Stock Scania truck and standard SCS curtainsider fallback "
                          "verified; client missing-content protection active."
                        : "[HeavyMP recovery] Client vehicle recovery verification FAILED; missing vehicle "
                          "safety is unavailable.");
    nativeRecoveryMounted = recovery;
    return nativeRecoveryMounted;
}
