// Match the native async contract: bytes appear ONLY after request completion.
#include "heavymp_content.cpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
namespace {
std::array<Ptr, 8> testMethods{};
struct TestFile {
    Ptr* methods = nullptr;
} testFile;
unsigned opens = 0, closes = 0, reads = 0, waits = 0, lastMode = 99, lastFilesystem = 99;
const unsigned char testBytes[] = {'H', 'e', 'a', 'v', 'y', 'M', 'a', 'p'};
bool failCompletion = false, rejectSubmit = false, pending = false, closedPending = false;
void* destination = nullptr;
constexpr Ptr requestId = 0x123456789abcdef0ULL;
void destroyTest(Ptr, unsigned) {
    closes++;
    if (pending)
        closedPending = true;
}
unsigned char metadataTest(Ptr, void* out) {
    const ULONGLONG size = sizeof(testBytes);
    memcpy(static_cast<unsigned char*>(out) + 16, &size, 8);
    return 1;
}
Ptr readTest(Ptr, void* out, size_t count, ULONGLONG offset, unsigned char waitable) {
    reads++;
    if (rejectSubmit || offset || count != sizeof(testBytes) || waitable != 1)
        return 0;
    destination = out;
    pending = true;
    return requestId;
}
unsigned char completeTest(Ptr, Ptr request) {
    waits++;
    if (!pending || request != requestId)
        return 0;
    pending = false;
    if (failCompletion)
        return 0;
    memcpy(destination, testBytes, sizeof(testBytes));
    return 1;
}
Ptr* openTest(Ptr* out, const char** path, unsigned mode, unsigned filesystem) {
    opens++;
    lastMode = mode;
    lastFilesystem = filesystem;
    *out = mode == 0 && filesystem == 1 && !strcmp(*path, "/map/europe.mbd")
               ? reinterpret_cast<Ptr>(&testFile)
               : 0;
    return out;
}
} // namespace
namespace {
std::map<std::string, std::vector<unsigned char>> recoveryFiles;
const std::vector<unsigned char>* recoveryBytes = nullptr;
void* recoveryDestination = nullptr;
void recoveryDestroy(Ptr, unsigned) {}
unsigned char recoveryMetadata(Ptr, void* out) {
    ULONGLONG size = recoveryBytes->size();
    memcpy(static_cast<unsigned char*>(out) + 16, &size, 8);
    return 1;
}
Ptr recoveryRead(Ptr, void* out, size_t count, ULONGLONG offset, unsigned char waitable) {
    if (offset || waitable != 1 || count != recoveryBytes->size())
        return 0;
    recoveryDestination = out;
    return requestId;
}
unsigned char recoveryComplete(Ptr, Ptr request) {
    if (request != requestId)
        return 0;
    memcpy(recoveryDestination, recoveryBytes->data(), recoveryBytes->size());
    return 1;
}
std::array<Ptr, 8> recoveryMethods{};
TestFile recoveryFile;
Ptr* recoveryOpen(Ptr* out, const char** path, unsigned mode, unsigned filesystem) {
    auto found = recoveryFiles.find(*path);
    *out = 0;
    if (mode == 0 && filesystem == 1 && found != recoveryFiles.end()) {
        recoveryBytes = &found->second;
        *out = reinterpret_cast<Ptr>(&recoveryFile);
    }
    return out;
}
} // namespace
int main() {
    const auto startingXp =
        HeavyMpCareerStarter::experienceForStartingLevel("level_xp[0]: 200\r\nlevel_xp[1]: 500\r\n");
    if (startingXp != 49700)
        return 30;
    std::string initial =
        "SiiNunit\r\n{\r\neconomy: .economy\r\n{\r\n new_game: true\r\n @include "
        "\"locked_screens.sui\"\r\n}\r\nbank: .bank\r\n{\r\n money_account: 2000\r\n}\r\n}\r\n";
    if (!HeavyMpCareerStarter::prepareSave(initial, startingXp) ||
        initial.find("experience_points: 49700") == std::string::npos ||
        initial.find("money_account: 1000000") == std::string::npos ||
        initial.find("locked_screens.sui") == std::string::npos)
        return 31;
    auto starter = HeavyMpCareerStarter::saveArchive(initial);
    if (get32(starter, 0) != 0x04034b50 ||
        get32(starter, static_cast<unsigned>(starter.size() - 22)) != 0x06054b50)
        return 32;
    initial.replace(initial.find("1000000"), 7, "2000000");
    if (!HeavyMpCareerStarter::prepareSave(initial, startingXp) ||
        initial.find("money_account: 2000000") == std::string::npos)
        return 33;
    // Exercise the real native parser: archives without map sectors must work,
    // including catalogues larger than the former 32-package client ceiling.
    for (unsigned count : {0u, 1u, 33u, 256u}) {
        std::vector<unsigned char> b(headerSize + count * recordSize);
        memcpy(b.data(), "HMCT", 4);
        put32(b, 4, 2);
        put32(b, 112, count);
        put32(b, 116, 3);
        put32(b, 120, static_cast<unsigned>(b.size()));
        if (!requestShape(b))
            return 10;
        auto truncated = b;
        truncated.pop_back();
        if (requestShape(truncated))
            return 11;
        put32(b, 124, 1);
        if (requestShape(b))
            return 12;
    }
    {
        std::vector<unsigned char> b(headerSize + 257 * recordSize);
        memcpy(b.data(), "HMCT", 4);
        put32(b, 4, 2);
        put32(b, 112, 257);
        put32(b, 116, 3);
        put32(b, 120, static_cast<unsigned>(b.size()));
        if (requestShape(b))
            return 13;
    }
    testMethods = {reinterpret_cast<Ptr>(&destroyTest),
                   reinterpret_cast<Ptr>(&metadataTest),
                   0,
                   reinterpret_cast<Ptr>(&readTest),
                   0,
                   0,
                   0,
                   reinterpret_cast<Ptr>(&completeTest)};
    testFile.methods = testMethods.data();
    unsigned char actualBytes[sizeof(testBytes)]{};
    ULONGLONG actual = 0;
    ReadDiagnostics diagnostic{};
    if (!readVirtualWith(&openTest, "/map/europe.mbd", actualBytes, sizeof(actualBytes), &actual,
                         &diagnostic) ||
        lastMode != 0 || lastFilesystem != 1 || actual != sizeof(testBytes) ||
        memcmp(actualBytes, testBytes, sizeof(testBytes)) || waits != 1 || closes != 1 ||
        diagnostic.request != requestId || diagnostic.stage != ReadStage::ready || closedPending)
        return 1;
    diagnostic = {};
    if (readVirtualWith(&openTest, "/map/europe.mbd", actualBytes, sizeof(actualBytes) - 1, &actual,
                        &diagnostic) ||
        reads != 1 || waits != 1 || closes != 2 || diagnostic.stage != ReadStage::size)
        return 2;
    failCompletion = true;
    diagnostic = {};
    if (readVirtualWith(&openTest, "/map/europe.mbd", actualBytes, sizeof(actualBytes), &actual,
                        &diagnostic) ||
        waits != 2 || closes != 3 || diagnostic.stage != ReadStage::complete || closedPending)
        return 3;
    rejectSubmit = true;
    diagnostic = {};
    if (readVirtualWith(&openTest, "/map/europe.mbd", actualBytes, sizeof(actualBytes), &actual,
                        &diagnostic) ||
        waits != 2 || closes != 4 || diagnostic.stage != ReadStage::submit)
        return 4;
    if (opens != 4 || lastMode != 0 || pending)
        return 5;
    recoveryMethods = {reinterpret_cast<Ptr>(&recoveryDestroy),
                       reinterpret_cast<Ptr>(&recoveryMetadata),
                       0,
                       reinterpret_cast<Ptr>(&recoveryRead),
                       0,
                       0,
                       0,
                       reinterpret_cast<Ptr>(&recoveryComplete)};
    recoveryFile.methods = recoveryMethods.data();
    const std::string source = __FILE__;
    const auto slash = source.find_last_of("/\\");
    const auto assets =
        (slash == std::string::npos ? std::string("native") : source.substr(0, slash)) + "/recovery";
    for (const auto& entry : nativeRecoveryEntries) {
        std::ifstream file(assets + entry.path, std::ios::binary);
        if (!file)
            return 20;
        recoveryFiles[entry.path] = std::vector<unsigned char>(std::istreambuf_iterator<char>(file), {});
    }
    if (!verifyNativeRecoveryWith(&recoveryOpen))
        return 21;
    const std::string trailer = "/def/vehicle/trailer/fallback.sii";
    recoveryFiles[trailer][0] ^= 1;
    if (verifyNativeRecoveryWith(&recoveryOpen))
        return 22;
    recoveryFiles.erase(trailer);
    if (verifyNativeRecoveryWith(&recoveryOpen))
        return 23;
    puts("PASS: real recovery VFS verification accepts stock truck/trailer bytes and rejects corrupt or "
         "missing trailer fallback.");
    puts("PASS: addon sessions without map probes, 256 packages, malformed requests rejected; read_only; "
         "opaque request awaited before hashing/close; size/submit/completion failures distinguished.");
    return 0;
}
