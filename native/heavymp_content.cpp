#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <cmath>
#include <cstdio>
#include "heavymp_notification_state.h"
#pragma comment(lib, "user32.lib")
#include <bcrypt.h>
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include "vendor/minhook/include/MinHook.h"
#include "heavymp_content.h"
#include "heavymp_career_starter.h"
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace {
using Ptr = std::uintptr_t;
#include "heavymp_game_build.h"
constexpr auto& buildHash = HeavyMpGameBuild::sha256;
constexpr unsigned headerSize = 128, recordSize = 40, maxPackages = 256;
// The engine can read these devices past SDK shutdown. Their lifetime is the
// process lifetime, as with base archives. Never destroy a still-mounted device.
std::vector<Ptr> mountedDevices;
std::vector<HANDLE> contentLocks;
bool initialized = false;
std::wstring exchangeRoot;
scs_log_t logger = nullptr;
unsigned get32(const std::vector<unsigned char>& b, unsigned at) {
    unsigned v = 0;
    memcpy(&v, b.data() + at, 4);
    return v;
}
ULONGLONG get64(const std::vector<unsigned char>& b, unsigned at) {
    ULONGLONG v = 0;
    memcpy(&v, b.data() + at, 8);
    return v;
}
void put32(std::vector<unsigned char>& b, unsigned at, unsigned v) {
    memcpy(b.data() + at, &v, 4);
}
ULONGLONG utc() {
    FILETIME t{};
    GetSystemTimeAsFileTime(&t);
    return ((ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000 - 11644473600000ULL;
}
ULONGLONG birth() {
    FILETIME a{}, b{}, c{}, d{};
    if (!GetProcessTimes(GetCurrentProcess(), &a, &b, &c, &d))
        return 0;
    return ((ULONGLONG(a.dwHighDateTime) << 32) | a.dwLowDateTime) / 10000 - 11644473600000ULL;
}
std::string hex(const unsigned char* digest, unsigned size = 32) {
    const char* digits = "0123456789abcdef";
    std::string s;
    for (unsigned i = 0; i < size; i++) {
        s += digits[digest[i] >> 4];
        s += digits[digest[i] & 15];
    }
    return s;
}
bool digest(HANDLE file, std::array<unsigned char, 32>& out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD size = 0, done = 0;
    bool okay =
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), 4, &done, 0) >= 0;
    std::vector<unsigned char> object(size), buffer(65536);
    if (okay)
        okay = BCryptCreateHash(alg, &hash, object.data(), size, nullptr, 0, 0) >= 0;
    LARGE_INTEGER zero{};
    if (okay)
        okay = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE;
    while (okay) {
        DWORD n = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr)) {
            okay = false;
            break;
        }
        if (!n)
            break;
        okay = BCryptHashData(hash, buffer.data(), n, 0) >= 0;
    }
    if (okay)
        okay = BCryptFinishHash(hash, out.data(), 32, 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return okay;
}
bool supported() {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768))
        return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    std::array<unsigned char, 32> h{};
    const bool ok = digest(f, h) && hex(h.data()) == buildHash;
    CloseHandle(f);
    return ok;
}
bool launcherAlive(unsigned pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p)
        return false;
    wchar_t image[32768]{};
    DWORD size = 32768, exit = 0;
    bool valid = QueryFullProcessImageNameW(p, 0, image, &size) && GetExitCodeProcess(p, &exit) &&
                 exit == STILL_ACTIVE;
    CloseHandle(p);
    const auto slash = wcsrchr(image, L'\\');
    return valid && slash && !_wcsicmp(slash + 1, L"HeavyMP.exe");
}
bool requestShape(const std::vector<unsigned char>& b) {
    if (b.size() < headerSize || b.size() > 32768 || memcmp(b.data(), "HMCT", 4) || get32(b, 4) != 2 ||
        get32(b, 112) > maxPackages || get32(b, 124) > 32 ||
        b.size() < headerSize + get32(b, 112) * recordSize || get32(b, 120) != b.size())
        return false;
    const auto flags = get32(b, 116);
    if (flags != 3 && (flags != 7 || !get32(b, 112)))
        return false;
    size_t offset = headerSize + get32(b, 112) * recordSize;
    for (unsigned i = 0; i < get32(b, 124); i++) {
        if (offset + 44 > b.size())
            return false;
        const auto n = get32(b, static_cast<unsigned>(offset));
        const auto length = get64(b, static_cast<unsigned>(offset + 4));
        if (n < 5 || n > 512 || !length || length > 8 * 1024 * 1024 || offset + 44 + n > b.size() ||
            memcmp(b.data() + offset + 44, "/map/", 5))
            return false;
        for (unsigned j = 0; j < n; j++)
            if (b[offset + 44 + j] < 32 || b[offset + 44 + j] > 126)
                return false;
        offset += 44 + n;
    }
    // Non-map addons and HashFS archives have no ZIP map-sector probes. Every
    // package still goes through its SHA256, native open and mount checks.
    return offset == b.size();
}
enum class LaunchRequest {
    ready,
    missing,
    unreadable,
    malformed,
    expired,
    launcherGone,
    unclaimed,
    otherProcess
};
LaunchRequest readRequestAt(const std::wstring& root, std::vector<unsigned char>& b) {
    HANDLE f = CreateFileW((root + L"content-request.bin").c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? LaunchRequest::missing
                                                                              : LaunchRequest::unreadable;
    }
    LARGE_INTEGER size{};
    DWORD n = 0;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= headerSize && size.QuadPart <= 32768;
    if (ok) {
        b.resize(static_cast<size_t>(size.QuadPart));
        ok = ReadFile(f, b.data(), static_cast<DWORD>(b.size()), &n, nullptr) && n == b.size();
    }
    CloseHandle(f);
    if (!ok || !requestShape(b))
        return LaunchRequest::malformed;
    const auto time = utc(), expires = get64(b, 24);
    if (expires < time || expires - time > 60000)
        return LaunchRequest::expired;
    return launcherAlive(get32(b, 12)) ? LaunchRequest::ready : LaunchRequest::launcherGone;
}
LaunchRequest waitForLaunchClaim(const std::wstring& root, std::vector<unsigned char>& request,
                                 DWORD timeout) {
    const auto initial = readRequestAt(root, request);
    if (initial != LaunchRequest::ready)
        return initial;
    const auto pid = GetCurrentProcessId();
    const auto born = birth();
    const auto until = GetTickCount64() + timeout;
    while (!get32(request, 8)) {
        if (GetTickCount64() >= until)
            return LaunchRequest::unclaimed;
        Sleep(25);
        std::vector<unsigned char> fresh;
        const auto status = readRequestAt(root, fresh);
        // A lease replacement can briefly be unavailable. Wait for the same
        // launcher and nonce, rather than abandoning a valid pending launch.
        if (status == LaunchRequest::missing || status == LaunchRequest::unreadable)
            continue;
        if (status != LaunchRequest::ready)
            return status;
        if (get32(fresh, 12) != get32(request, 12) || memcmp(fresh.data() + 32, request.data() + 32, 16))
            return LaunchRequest::otherProcess;
        request = std::move(fresh);
    }
    return get32(request, 8) == pid && get64(request, 16) == born ? LaunchRequest::ready
                                                                  : LaunchRequest::otherProcess;
}
void receipt(std::vector<unsigned char> b, unsigned error) {
    memcpy(b.data(), error ? "HMCE" : "HMCO", 4);
    put32(b, 124, error);
    const auto tmp = exchangeRoot + L"content-response.tmp", target = exchangeRoot + L"content-response.bin";
    HANDLE f =
        CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD n = 0;
    const bool okay = WriteFile(f, b.data(), static_cast<DWORD>(b.size()), &n, nullptr) && n == b.size();
    CloseHandle(f);
    if (okay)
        MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    else
        DeleteFileW(tmp.c_str());
}
bool arguments() {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args)
        return false;
    bool pure = false, workshop = false, invalid = false;
    for (int i = 1; i < count; i++) {
        pure = pure || !_wcsicmp(args[i], L"-pure");
        workshop = workshop || !_wcsicmp(args[i], L"-noworkshop");
        for (auto bad : {L"-force_mods", L"-homedir", L"-mount", L"-rpath", L"-edit", L"-preview"})
            if (!_wcsicmp(args[i], bad))
                invalid = true;
    }
    LocalFree(args);
    return pure && workshop && !invalid;
}
bool regular(const std::wstring& path) {
    const auto a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}
bool plainDirectory(const std::wstring& path) {
    const auto a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}
std::string utf8(const std::wstring& path) {
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
                                      static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), s.data(),
                        n, nullptr, nullptr);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}
// POD-only boundaries keep SEH separate from scopes owning C++ destructors.
bool codeMatches(Ptr e) {
    const unsigned char a[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
                               0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x8b};
    const unsigned char b[] = {0x48, 0x83, 0xec, 0x48, 0x48, 0x63, 0xd1, 0x48,
                               0x3b, 0x15, 0x12, 0xe7, 0x63, 0x02, 0x73, 0x10};
    const unsigned char c[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74,
                               0x24, 0x20, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56};
    const unsigned char d[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
                               0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41};
    __try {
        return !memcmp(reinterpret_cast<void*>(e + 0x111820), a, 16) &&
               !memcmp(reinterpret_cast<void*>(e + 0x156db0), b, 16) &&
               !memcmp(reinterpret_cast<void*>(e + 0x15d1b0), c, 16) &&
               !memcmp(reinterpret_cast<void*>(e + 0x114a30), d, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool openArchive(Ptr e, const char* path, Ptr* device) {
    __try {
        // Type 3 detects HashFS and falls back to ZIP. Path views are const char**.
        reinterpret_cast<Ptr* (*)(Ptr*, unsigned, const char**, unsigned char, int, unsigned)>(e + 0x111820)(
            device, 3, &path, 1, 0, 0xffffffffu);
        return *device && *reinterpret_cast<unsigned char*>(*device + 0x51) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool mountArchive(Ptr e, Ptr device, int priority) {
    // The native mount ABI expects a prefix WITHOUT the virtual path's leading
    // slash. Root archives (base/DLC/profile mods) use an empty string, not "/".
    // A slash is accepted by mount(), but its files then fail normal VFS lookup.
    const char* root = "";
    __try {
        // Additional ZIP packages use the mod overlay, rather than the SCS pool
        // indexed for base/DLC archive access. No profile mod-manager entry is made.
        const auto pool = reinterpret_cast<Ptr (*)(unsigned)>(e + 0x156db0)(2);
        if (!pool)
            return false;
        return reinterpret_cast<unsigned char (*)(Ptr, Ptr, const char**, int, unsigned char, unsigned char)>(
                   e + 0x15d1b0)(pool, device, &root, priority, 0, 0) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using OpenVirtual = Ptr* (*)(Ptr*, const char**, unsigned, unsigned);
enum class ReadStage : unsigned { open, metadata, size, submit, complete, ready, fault };
struct ReadDiagnostics {
    ReadStage stage = ReadStage::open;
    Ptr request = 0;
};
const char* readStageName(ReadStage s) {
    switch (s) {
    case ReadStage::open:
        return "open_failed";
    case ReadStage::metadata:
        return "metadata_failed";
    case ReadStage::size:
        return "size_mismatch";
    case ReadStage::submit:
        return "request_failed";
    case ReadStage::complete:
        return "completion_failed";
    case ReadStage::ready:
        return "read_ready";
    default:
        return "native_fault";
    }
}
bool readVirtualWith(OpenVirtual open, const char* path, unsigned char* bytes, size_t expected,
                     ULONGLONG* actual, ReadDiagnostics* diagnostic) {
    Ptr file = 0;
    bool okay = false;
    unsigned char metadata[56]{};
    __try {
        // Native enum: 0=read_only, 1=create_new, 2=create_truncate, 3=read_write.
        // The fourth argument selects virtual filesystem (1), not the open mode.
        open(&file, &path, 0, 1);
        if (file) {
            const auto vt = *reinterpret_cast<Ptr*>(file);
            diagnostic->stage = ReadStage::metadata;
            if (reinterpret_cast<unsigned char (*)(Ptr, void*)>(*reinterpret_cast<Ptr*>(vt + 8))(file,
                                                                                                 metadata)) {
                memcpy(actual, metadata + 16, 8);
                diagnostic->stage = ReadStage::size;
                if (*actual == expected) {
                    diagnostic->stage = ReadStage::submit;
                    // Native file::read returns an opaque request identifier, not a byte count.
                    // Match the HashFS header reader: submit(...,0,1), then complete(id).
                    diagnostic->request =
                        reinterpret_cast<Ptr (*)(Ptr, void*, size_t, ULONGLONG, unsigned char)>(
                            *reinterpret_cast<Ptr*>(vt + 0x18))(file, bytes, expected, 0, 1);
                    if (diagnostic->request) {
                        diagnostic->stage = ReadStage::complete;
                        okay = reinterpret_cast<unsigned char (*)(Ptr, Ptr)>(
                                   *reinterpret_cast<Ptr*>(vt + 0x38))(file, diagnostic->request) != 0;
                        if (okay)
                            diagnostic->stage = ReadStage::ready;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diagnostic->stage = ReadStage::fault;
        okay = false;
    }
    if (file) {
        __try {
            reinterpret_cast<void (*)(Ptr, unsigned)>(**reinterpret_cast<Ptr**>(file))(file, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            diagnostic->stage = ReadStage::fault;
            okay = false;
        }
    }
    return okay;
}
bool readVirtual(Ptr e, const char* path, unsigned char* bytes, size_t expected, ULONGLONG* actual,
                 ReadDiagnostics* diagnostic) {
    return readVirtualWith(reinterpret_cast<OpenVirtual>(e + 0x114a30), path, bytes, expected, actual,
                           diagnostic);
}
bool bytesDigest(std::vector<unsigned char>& bytes, std::array<unsigned char, 32>& hash) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    const bool okay =
        BCryptHash(alg, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()), hash.data(), 32) >= 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    return okay;
}
bool verifyProbes(Ptr e, const std::vector<unsigned char>& b) {
    unsigned offset = headerSize + get32(b, 112) * recordSize;
    for (unsigned i = 0; i < get32(b, 124); i++) {
        const auto length = get32(b, offset);
        const auto size = get64(b, offset + 4);
        const std::string path(reinterpret_cast<const char*>(b.data() + offset + 44), length);
        std::vector<unsigned char> bytes(static_cast<size_t>(size));
        std::array<unsigned char, 32> actualHash{};
        ULONGLONG actualSize = 0;
        ReadDiagnostics diagnostic{};
        const bool read = readVirtual(e, path.c_str(), bytes.data(), bytes.size(), &actualSize, &diagnostic);
        const bool hashed = read && bytesDigest(bytes, actualHash);
        const bool okay = hashed && !memcmp(actualHash.data(), b.data() + offset + 12, 32);
        const auto stage = read ? (hashed ? (okay ? "verified" : "hash_mismatch") : "hash_failed")
                                : readStageName(diagnostic.stage);
        if (logger) {
            const auto m =
                std::string(okay ? "[HeavyMP content] VFS verified " : "[HeavyMP content] VFS FAILED ") +
                path + "; stage=" + stage + " request=" + std::to_string(diagnostic.request) +
                " expectedSize=" + std::to_string(size) + " actualSize=" + std::to_string(actualSize) +
                " expectedSha=" + hex(b.data() + offset + 12) +
                " actualSha=" + (hashed ? hex(actualHash.data()) : "not_read");
            logger(okay ? SCS_LOG_TYPE_message : SCS_LOG_TYPE_error, m.c_str());
        }
        if (!okay)
            return false;
        offset += 44 + length;
    }
    return true;
}
#include "heavymp_recovery_content.inl"
#include "heavymp_hud.inl"

std::string readStarterFile(Ptr engine, const char* path) {
    ULONGLONG size = 0;
    ReadDiagnostics diagnostic{};
    readVirtual(engine, path, nullptr, 0, &size, &diagnostic);
    if (diagnostic.stage != ReadStage::size || size < 8 || size > 65536)
        return {};
    std::string text(static_cast<size_t>(size), '\0');
    ULONGLONG actual = 0;
    return readVirtual(engine, path, reinterpret_cast<unsigned char*>(text.data()), text.size(), &actual,
                       &diagnostic)
               ? text
               : std::string{};
}

bool mountCareerStarter(Ptr engine) {
    using namespace HeavyMpCareerStarter;
    auto save = readStarterFile(engine, savePath);
    const auto experience = experienceForStartingLevel(readStarterFile(engine, xpPath));
    if (!prepareSave(save, experience))
        return false;
    auto archive = saveArchive(save);
    std::array<unsigned char, 32> archiveDigest{};
    if (!bytesDigest(archive, archiveDigest))
        return false;
    const auto name = hex(archiveDigest.data());
    const auto path =
        exchangeRoot + L"native-ui\\starter-" + std::wstring(name.begin(), name.end()) + L".scs";
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const auto size = static_cast<DWORD>(archive.size());
        const bool good = WriteFile(file, archive.data(), size, &written, nullptr) && written == size;
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
    const auto text = utf8(path);
    Ptr device = 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart != static_cast<LONGLONG>(archive.size()) ||
        !digest(file, actual) || actual != archiveDigest || text.empty() ||
        !openArchive(engine, text.c_str(), &device)) {
        CloseHandle(file);
        return false;
    }
    mountedDevices.push_back(device);
    if (!mountArchive(engine, device, 10001)) {
        CloseHandle(file);
        return false;
    }
    contentLocks.push_back(file);
    if (readStarterFile(engine, savePath) != save)
        return false;
    if (logger)
        logger(SCS_LOG_TYPE_message,
               "[HeavyMP career] Initial save verified: level 100 and at least EUR 1,000,000.");
    return true;
}

unsigned load(const std::vector<unsigned char>& b) {
    if (!supported() || hex(b.data() + 80) != buildHash)
        return 101;
    if (!arguments())
        return 102;
    const auto e = reinterpret_cast<Ptr>(GetModuleHandleW(nullptr));
    if (!codeMatches(e))
        return 106;
    const auto store = exchangeRoot + L"content", objects = store + L"\\objects";
    if (get32(b, 112) && (!plainDirectory(store) || !plainDirectory(objects)))
        return 103;
    std::vector<std::string> paths;
    // Verify every object before any mount; hold read locks for the session.
    for (unsigned i = 0; i < get32(b, 112); i++) {
        const auto r = headerSize + i * recordSize;
        const auto name = hex(b.data() + r);
        const auto path = objects + L"\\" + std::wstring(name.begin(), name.end()) + L".scs";
        if (!regular(path))
            return 103;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return 103;
        LARGE_INTEGER size{};
        std::array<unsigned char, 32> actual{};
        const bool okay = GetFileSizeEx(f, &size) && size.QuadPart >= 22 &&
                          static_cast<ULONGLONG>(size.QuadPart) == get64(b, r + 32) && digest(f, actual) &&
                          !memcmp(actual.data(), b.data() + r, 32);
        if (!okay) {
            CloseHandle(f);
            return 103;
        }
        contentLocks.push_back(f);
        const auto text = utf8(path);
        if (text.empty())
            return 103;
        paths.push_back(text);
    }
    for (unsigned i = 0; i < paths.size(); i++) {
        Ptr device = 0;
        if (!openArchive(e, paths[i].c_str(), &device))
            return 104;
        mountedDevices.push_back(device);
        if (!mountArchive(e, device, 5000 + static_cast<int>(i)))
            return 105;
        if (logger) {
            const auto m = std::string("[HeavyMP content] Mounted verified object ") +
                           hex(b.data() + headerSize + i * recordSize) + " from HeavyMP store (order " +
                           std::to_string(i) + ").";
            logger(SCS_LOG_TYPE_message, m.c_str());
        }
    }
    // Keep the recovery definitions above older server repair packs.
    if (!mountNativeRecovery(e))
        return 112;
    if (!verifyProbes(e, b))
        return 110;
    if ((get32(b, 116) & 4) && !mountCareerStarter(e) && logger)
        logger(SCS_LOG_TYPE_warning,
               "[HeavyMP career] Starter bonus unavailable for this map. Continuing with its initial save.");
    return 0;
}
} // namespace

unsigned heavymp_content_initialize(scs_log_t log) {
    logger = log;
    if (initialized)
        return 0;
    wchar_t appdata[32768]{};
    const auto n = GetEnvironmentVariableW(L"APPDATA", appdata, 32768);
    if (!n || n >= 32768)
        return 0;
    exchangeRoot = std::wstring(appdata) + L"\\heavymp-launcher\\";
    std::vector<unsigned char> request;
    const auto claim = waitForLaunchClaim(exchangeRoot, request, 15000);
    if (claim != LaunchRequest::ready) {
        if (logger && claim != LaunchRequest::missing) {
            const char* reason = "request could not be read";
            switch (claim) {
            case LaunchRequest::malformed:
                reason = "invalid request format";
                break;
            case LaunchRequest::expired:
                reason = "request expired";
                break;
            case LaunchRequest::launcherGone:
                reason = "launcher process unavailable";
                break;
            case LaunchRequest::unclaimed:
                reason = "launcher did not claim ETS2";
                break;
            case LaunchRequest::otherProcess:
                reason = "request belongs to another launch or ETS2 process";
                break;
            default:
                break;
            }
            const auto message = std::string("[HeavyMP content] Direct mounting skipped: ") + reason + ".";
            logger(SCS_LOG_TYPE_warning, message.c_str());
        }
        return 0;
    }
    initialized = true;
    unsigned error = 109;
    try {
        error = load(request);
    } catch (...) {
        error = 109;
    }
    if (!error && !installNativeHud(reinterpret_cast<Ptr>(GetModuleHandleW(nullptr))) && logger)
        logger(SCS_LOG_TYPE_warning, "[HeavyMP HUD] Stock notification API unavailable.");
    receipt(request, error);
    if (logger) {
        const auto m = error
                           ? std::string("[HeavyMP content] Direct mounting failed, code ") +
                                 std::to_string(error) + "; multiplayer suspended."
                           : std::string("[HeavyMP content] Direct content ready; personal mods and Workshop "
                                         "disabled. Revision ") +
                                 hex(request.data() + 48) + ". Session " + hex(request.data() + 32, 16) + ".";
        logger(error ? SCS_LOG_TYPE_error : SCS_LOG_TYPE_message, m.c_str());
    }
    return error ? 2 : 1;
}

extern "C" __declspec(dllexport) BOOL HeavyMpNativeNotification(const wchar_t* text, unsigned official) {
    return enqueueNativeNotice(text, official != 0) ? TRUE : FALSE;
}
