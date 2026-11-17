// ETS2 1.62.3.3 native map batch interface, verified against the local binary.
// Included inside heavymp_native.cpp's namespace to share guards and ownership.
struct MapRecord {
    Ptr unit = 0;
    float x = 0, y = 0, z = 0;
    unsigned category = 26, icon = 22, flags = 0;
    Ptr extra = 0;
};
static_assert(sizeof(MapRecord) == 40);
struct MapBatch {
    Ptr allocator = 0;
    MapRecord* data = nullptr;
    size_t count = 0, capacity = 0;
};
using MapIcons = void (*)(Ptr, Ptr);
using MapDraw = void (*)(Ptr, Ptr, Ptr);
MapIcons originalMapIcons = nullptr;
MapDraw originalMapDraw = nullptr;
void* mapIconsAddress = nullptr;
void* mapDrawAddress = nullptr;
bool mapReady = false, mapFault = false;
std::array<MapRecord, 32> mapRecords{};
unsigned mapCount = 0;
SOCKET mapInput = INVALID_SOCKET;
ULONGLONG mapAt = 0;
ULONGLONG mapDiagnosticAt = 0;
bool mapWidget(Ptr widget) {
    if (!readable(widget, 0x890))
        return false;
    const Ptr table = field<Ptr>(widget, 0);
    return table == engine + 0x23d9030 || table == engine + 0x23b32c8 || table == engine + 0x23d8e18;
}
void updateMap(const HeavyMpNativePeer* peers, unsigned count) {
    mapCount = 0;
    for (unsigned i = 0; peers && i < std::min(count, 32u); ++i) {
        const auto& p = peers[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.z) || std::abs(p.x) > 1e7 || std::abs(p.z) > 1e7)
            continue;
        auto& record = mapRecords[mapCount++];
        record = {};
        record.x = static_cast<float>(p.x);
        record.z = static_cast<float>(p.z);
    }
}
bool receiveMapPacket(const unsigned char* data, size_t bytes) {
    if (bytes < 16 || memcmp(data, "HMM1", 4))
        return false;
    unsigned count = 0, stride = 0;
    memcpy(&count, data + 8, 4);
    memcpy(&stride, data + 12, 4);
    if (count > 32 || stride != 72 || bytes != 16 + count * 72)
        return false;
    std::array<MapRecord, 32> next{};
    for (unsigned i = 0; i < count; ++i) {
        const auto* record = data + 16 + i * 72;
        double pose[6]{};
        unsigned age = 0, flags = 0;
        memcpy(pose, record + 16, 48);
        memcpy(&age, record + 64, 4);
        memcpy(&flags, record + 68, 4);
        if (age > 1000 || flags)
            return false;
        for (unsigned j = 0; j < 6; ++j)
            if (!std::isfinite(pose[j]) || std::abs(pose[j]) > (j == 0 || j == 2 ? 1e7 : j == 1 ? 1e5 : 10))
                return false;
        next[i].x = static_cast<float>(pose[0]);
        next[i].z = static_cast<float>(pose[2]);
    }
    mapRecords = next;
    mapCount = count;
    mapAt = GetTickCount64();
    return true;
}
void receiveMap() {
    if (mapInput == INVALID_SOCKET)
        return;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned char packet[2400]{};
        sockaddr_in remote{};
        int length = sizeof(remote);
        const int bytes = recvfrom(mapInput, reinterpret_cast<char*>(packet), sizeof(packet), 0,
                                   reinterpret_cast<sockaddr*>(&remote), &length);
        if (bytes < 0)
            break;
        if (remote.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
            receiveMapPacket(packet, static_cast<size_t>(bytes));
    }
    if (GetTickCount64() - mapAt > 1000)
        mapCount = 0;
}
void addMapIcons(Ptr widget, Ptr painter) {
    if (!enabled || faulted || !mapReady || mapFault || !mapWidget(widget) || !mapCount ||
        !readable(painter, 0x40))
        return;
    const Ptr commands = field<Ptr>(painter, 0);
    if (!readable(commands, 0x28))
        return;
    const Ptr before = field<Ptr>(commands, 0x20);
    MapBatch batch{0, mapRecords.data(), mapCount, mapCount};
    const char* material = "/material/ui/map/truck_ico.mat";
    const float halfSize[2] = {16.f, 16.f};
    // The engine transforms world positions through this widget's own matrix
    // and clips icons to its view. No guessed screen coordinates are involved.
    call<void>(0xf60a50, widget, painter, &batch, &material, engine + 0x2e2ed70, halfSize, 1.f);
    const Ptr after = field<Ptr>(commands, 0x20);
    const auto now = GetTickCount64();
    if (!mapDiagnosticAt || now - mapDiagnosticAt >= 5000) {
        mapDiagnosticAt = now;
        char message[200]{};
        sprintf_s(message,
                  "[HeavyMP map] widget=%llx peers=%u command-bytes=%llu; visual verification required.",
                  field<Ptr>(widget, 0) - engine, mapCount, after >= before ? after - before : 0);
        log(message);
    }
    if (after >= before && after - before <= mapCount * 0x60 && readable(before, after - before)) {
        for (Ptr command = before; command < after; command += 0x60)
            if (field<short>(command, 0x54) == -1)
                field<unsigned>(command, 0x50) = 0xff42d984;
    }
}
bool guardedMapIcons(Ptr widget, Ptr painter) {
    __try {
        addMapIcons(widget, painter);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void mapIcons(Ptr widget, Ptr painter) {
    originalMapIcons(widget, painter);
    AcquireSRWLockExclusive(&nativeLock);
    if (!guardedMapIcons(widget, painter)) {
        mapFault = true;
        log("[HeavyMP map] Marker update fault; map markers disabled, native vehicles preserved.");
    }
    ReleaseSRWLockExclusive(&nativeLock);
}
bool guardedMapRefresh(Ptr widget) {
    __try {
        if (enabled && !faulted && mapReady && !mapFault && mapWidget(widget)) {
            // Full map, route advisor and cabin GPS cache their icon commands.
            // Invalidate only these map widgets so moved/removed peers are refreshed.
            field<unsigned char>(widget, 0x370) = 1;
            field<unsigned>(widget, 0x888) |= 0x400000;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void mapDraw(Ptr widget, Ptr painter, Ptr frame) {
    AcquireSRWLockExclusive(&nativeLock);
    receiveMap();
    if (!guardedMapRefresh(widget))
        mapFault = true;
    ReleaseSRWLockExclusive(&nativeLock);
    originalMapDraw(widget, painter, frame);
}
bool attachMap() {
    if (field<std::uint64_t>(engine, 0xf64230) != 0x5541575655dc8b4cULL ||
        field<std::uint64_t>(engine, 0xf67670) != 0x41575510245c8948ULL ||
        field<unsigned>(engine, 0xf60a50) != 0x4ddc8b4c)
        return false;
    mapIconsAddress = reinterpret_cast<void*>(engine + 0xf64230);
    mapDrawAddress = reinterpret_cast<void*>(engine + 0xf67670);
    if (!originalMapIcons && MH_CreateHook(mapIconsAddress, reinterpret_cast<void*>(&mapIcons),
                                           reinterpret_cast<void**>(&originalMapIcons)) != MH_OK)
        return false;
    if (!originalMapDraw && MH_CreateHook(mapDrawAddress, reinterpret_cast<void*>(&mapDraw),
                                          reinterpret_cast<void**>(&originalMapDraw)) != MH_OK)
        return false;
    if (MH_EnableHook(mapIconsAddress) != MH_OK)
        return false;
    if (MH_EnableHook(mapDrawAddress) != MH_OK) {
        MH_DisableHook(mapIconsAddress);
        return false;
    }
    mapInput = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mapInput == INVALID_SOCKET) {
        MH_DisableHook(mapDrawAddress);
        MH_DisableHook(mapIconsAddress);
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(mapInput, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive),
               sizeof(exclusive));
    unsigned short port = 34064;
    char text[16]{};
    if (GetEnvironmentVariableA("HEAVYMP_MAP_PORT", text, sizeof(text))) {
        char* end = nullptr;
        const auto value = strtoul(text, &end, 10);
        if (end && !*end && value > 0 && value <= 65535)
            port = static_cast<unsigned short>(value);
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(port);
    u_long nonblocking = 1;
    if (bind(mapInput, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
        ioctlsocket(mapInput, FIONBIO, &nonblocking) != 0) {
        closesocket(mapInput);
        mapInput = INVALID_SOCKET;
        MH_DisableHook(mapDrawAddress);
        MH_DisableHook(mapIconsAddress);
        return false;
    }
    return true;
}
