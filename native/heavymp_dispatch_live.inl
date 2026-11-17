// ETS2 1.62.3.3s only. Reuse the engine's own 'cheat job' freight path.
// Requests arrive on loopback and execute on the validated local actor thread.
SRWLOCK dispatchLiveLock = SRWLOCK_INIT;
struct LiveFreightRequest {
    std::array<unsigned char, 16> id{};
    char cargo[128]{}, source[128]{}, target[128]{}, trailer[128]{};
    unsigned units = 0, distance = 0;
    ULONGLONG at = 0;
};
LiveFreightRequest dispatchLivePending{};
bool dispatchLiveQueued = false, dispatchLiveOkay = false;
ULONGLONG dispatchLiveDone = 0;
bool dispatchLiveProcessing = false;
bool dispatchLiveOwned = false;
unsigned dispatchLiveUnits = 0;
std::array<unsigned char, 16> dispatchLiveDoneId{};
unsigned dispatchLiveFailure = 0;
std::atomic<ULONGLONG> dispatchLiveEligibleAt{0};
bool liveFreightToken(const char* value) {
    const auto n = strnlen_s(value, 128);
    if (!n || n > 120)
        return false;
    bool dot = true;
    for (size_t i = 0; i < n; ++i) {
        const char c = value[i];
        if (c == '.') {
            if (dot)
                return false;
            dot = true;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
            dot = false;
        }
    }
    return !dot;
}
struct LiveFreightString {
    Ptr vtable;
    const char* text;
    unsigned length, capacity;
    Ptr owner = 0;
};
static_assert(sizeof(LiveFreightString) == 32);
Ptr liveFreightUnit(const char* name) {
    LiveFreightString view{engine + 0x22217a0, name, static_cast<unsigned>(strlen(name)),
                           static_cast<unsigned>(strlen(name) + 1)};
    return call<Ptr>(0x12dae0, &view.text);
}
bool liveFreightTargetMatches(Ptr offer, const char* target) {
    // job_offer_data uses an inline-capacity string (2235b98), not exclusively
    // the dynamic string type accepted by the vehicle appearance reader.
    const Ptr value = offer + 0x10;
    if (!readable(value, 24))
        return false;
    const auto vtable = field<Ptr>(value, 0);
    if (vtable != engine + 0x22852f0 && vtable != engine + 0x22217a0)
        return false;
    const auto size = field<unsigned>(value, 16);
    const auto expected = strlen(target);
    const Ptr text = field<Ptr>(value, 8);
    return size == expected && size <= 120 && readable(text, size) &&
           !memcmp(reinterpret_cast<const void*>(text), target, size);
}
bool liveCompanyReady(Ptr company) {
    if (!readable(company, 0x108))
        return false;
    const Ptr runtime = field<Ptr>(company, 0x10);
    return readable(runtime, 0x108) && field<Ptr>(runtime, 0) == engine + 0x22e92e8;
}
std::string liveCompanyToken(std::uint64_t value) {
    std::string out;
    const char* alphabet = "0123456789abcdefghijklmnopqrstuvwxyz_";
    while (value && out.size() < 13) {
        const auto digit = value % 38;
        if (!digit)
            return {};
        out += alphabet[digit - 1];
        value /= 38;
    }
    return value ? std::string{} : out;
}
bool liveCompanyName(Ptr unit, std::string& out) {
    const unsigned handle = field<unsigned>(unit, 0xc), index = handle & 0xffff,
                   poolIndex = (handle >> 20) & 0x7ff;
    if (!handle || (handle & 0x80000000) || poolIndex >= 2040)
        return false;
    const Ptr pool = field<Ptr>(engine, 0x30ac380 + poolIndex * 8);
    if (!readable(pool, 0x90) || field<unsigned char>(pool, 0x88) != 4 || index >= field<size_t>(pool, 0x50))
        return false;
    const Ptr names = field<Ptr>(pool, 0), units = field<Ptr>(pool, 0x48);
    if (!readable(names + index * 32, 32) || !readable(units + index * 8, 8) ||
        field<Ptr>(units, index * 8) != unit)
        return false;
    if (liveCompanyToken(field<std::uint64_t>(names, index * 32)) != "company" ||
        liveCompanyToken(field<std::uint64_t>(names, index * 32 + 8)) != "volatile")
        return false;
    out = liveCompanyToken(field<std::uint64_t>(names, index * 32 + 16)) + "." +
          liveCompanyToken(field<std::uint64_t>(names, index * 32 + 24));
    return liveFreightToken(out.c_str());
}
bool buildLiveCompanyExclusions(std::string* out) {
    if (!enabled || faulted)
        return false;
    const Ptr economy = field<Ptr>(engine, 0x30aa900);
    if (!readable(economy, 0x38) || field<Ptr>(economy, 0) != engine + 0x229d3d8)
        return false;
    const auto count = field<size_t>(economy, 0x30);
    const Ptr list = field<Ptr>(economy, 0x28);
    if (count > 5000 || !readable(list, count * 8))
        return false;
    *out = "[";
    unsigned found = 0;
    for (size_t i = 0; i < count && found < 512; ++i) {
        const Ptr company = field<Ptr>(list, i * 8);
        if (!readable(company, 0x108) || field<Ptr>(company, 0) != engine + 0x2300200 ||
            liveCompanyReady(company))
            continue;
        std::string name;
        if (liveCompanyName(company, name)) {
            if (found++)
                *out += ",";
            *out += '"' + name + '"';
        }
    }
    *out += "]";
    return true;
}
bool acceptLiveDelivery(Ptr economy, Ptr source, Ptr offer, bool owned) {
    dispatchLiveFailure = 12;
    if (!liveCompanyReady(source))
        return false;
    dispatchLiveFailure = 2;
    if (field<std::uint64_t>(engine, 0x7890f0) != 0x482048894cc48b48ULL ||
        field<std::uint64_t>(engine, 0x7899f0) != 0x5541544155c48b48ULL)
        return false;
    dispatchLiveFailure = 13;
    const char* stayHere = "";
    const bool accepted = owned ? call<unsigned char>(0x7899f0, economy, source, offer) != 0
                                : call<unsigned char>(0x7890f0, economy, source, offer, &stayHere, 0u) != 0;
    if (!accepted)
        return false;
    dispatchLiveFailure = 0;
    return true;
}
const char* liveFreightReason() {
    switch (dispatchLiveFailure) {
    case 1:
        return "Stop your truck and finish or cancel your active job first.";
    case 2:
        return "This ETS2 build does not support live freight creation.";
    case 3:
        return "The departure, destination or cargo is unavailable on this map.";
    case 4:
        return "No native freight offer is available at this departure.";
    case 5:
        return "This trailer configuration is not available for this cargo.";
    case 6:
        return "Detach your owned trailer before creating a freight-market delivery.";
    case 7:
        return "The game did not confirm the new freight offer.";
    case 8:
        return "Live freight creation was disabled after a native fault. Restart ETS2.";
    case 9:
        return "The live delivery request expired.";
    case 10:
        return "The source or destination company is not available in the loaded map.";
    case 11:
        return "Your owned trailer is not ready or is incompatible with this cargo. Choose a compatible "
               "cargo.";
    case 12:
        return "This company is no longer available in the active map. Choose another departure or "
               "destination.";
    case 13:
        return "The game refused to accept the delivery. For owned trailers, park at the source loading "
               "area.";
    default:
        return "";
    }
}
bool createLiveFreight(const LiveFreightRequest& request) {
    dispatchLiveOwned = false;
    dispatchLiveUnits = 0;
    dispatchLiveFailure = 1;
    if (!dispatchLiveEligibleAt || GetTickCount64() - dispatchLiveEligibleAt > 500)
        return false;
    dispatchLiveFailure = 2;
    if (field<std::uint64_t>(engine, 0x4ab6d0) != 0x4c894c08245c8948ULL ||
        field<std::uint64_t>(engine, 0x12dae0) != 0x90ec81485640ULL ||
        field<std::uint64_t>(engine, 0x3bd2f0) != 0x8b4c20ec83485340ULL)
        return false;
    const Ptr economy = field<Ptr>(engine, 0x30aa900);
    if (!readable(economy, 0x1a0) || field<Ptr>(economy, 0) != engine + 0x229d3d8)
        return false;
    const Ptr player = field<Ptr>(economy, 0x18);
    if (!readable(player, 0xa8))
        return false;
    char cargoName[160]{}, sourceName[160]{}, targetName[160]{};
    sprintf_s(cargoName, "%s", request.cargo);
    sprintf_s(sourceName, "company.volatile.%s", request.source);
    sprintf_s(targetName, "company.volatile.%s", request.target);
    const Ptr cargo = liveFreightUnit(cargoName), source = liveFreightUnit(sourceName),
              target = liveFreightUnit(targetName), definition = liveFreightUnit(request.trailer);
    dispatchLiveFailure = 3;
    if (!readable(cargo, 0xe8) || field<Ptr>(cargo, 0) != engine + 0x234efd0 || field<LONG>(cargo, 8) >= 0 ||
        !readable(source, 0x120) || field<Ptr>(source, 0) != engine + 0x2300200 ||
        field<LONG>(source, 8) >= 0 || !readable(target, 0x120) ||
        field<Ptr>(target, 0) != engine + 0x2300200 || field<LONG>(target, 8) >= 0 || source == target)
        return false;
    // The 2026-10-25 crash dereferenced target->runtime + 0xf8 when runtime
    // was null. Registry and map-link presence alone do not make a company usable.
    dispatchLiveFailure = 12;
    if (!liveCompanyReady(source) || !liveCompanyReady(target))
        return false;
    // A profile can retain companies from removed maps. The engine's map helper
    // has an assertion path when that link is missing; never invoke it blindly.
    dispatchLiveFailure = 10;
    for (const auto company : {source, target}) {
        const Ptr link = field<Ptr>(company, 0x100);
        if (!readable(link, 0x168) || field<LONG>(link, 8) >= 0 || !field<Ptr>(link, 0x160))
            return false;
    }
    // Mirror the engine's owned-trailer selection, then validate its cargo-market
    // prerequisites before entering a branch which otherwise asserts on no trailer.
    Ptr ownTrailer = 0;
    const Ptr owned = field<Ptr>(player, 0xa0);
    if (field<unsigned>(player, 0x98) == 0 && readable(owned, 0x50) && field<LONG>(owned, 8) < 0) {
        const Ptr t = field<Ptr>(owned, 0x48);
        if (readable(t, 0x1a8) && field<LONG>(t, 8) < 0)
            ownTrailer = t;
    }
    dispatchLiveOwned = ownTrailer != 0;
    Ptr offers = 0, offer = 0, pair = 0;
    if (dispatchLiveOwned) {
        dispatchLiveFailure = 11;
        const Ptr ownDefinition = field<Ptr>(ownTrailer, 0x1a0);
        if (!readable(ownDefinition, 0x50) || field<LONG>(ownDefinition, 8) >= 0)
            return false;
        const auto n = field<size_t>(cargo, 0xb0);
        const Ptr allowed = field<Ptr>(cargo, 0xa8);
        if (!n || n > 64 || !readable(allowed, n * 8))
            return false;
        bool compatible = false;
        for (size_t i = 0; i < n; ++i)
            if (field<Ptr>(allowed, i * 8) == field<Ptr>(ownDefinition, 0x48))
                compatible = true;
        if (!compatible)
            return false;
        const auto assignedCount = field<size_t>(player, 0x80);
        const Ptr assigned = field<Ptr>(player, 0x78);
        if (!assignedCount || assignedCount > 10 || !readable(assigned, assignedCount * 8))
            return false;
        const Ptr slot = field<Ptr>(assigned, 0);
        if (!readable(slot, 0x50) || field<LONG>(slot, 8) >= 0 || field<Ptr>(slot, 0x48) != ownTrailer)
            return false;
        const Ptr permanent = field<Ptr>(source, 0x68);
        if (!readable(permanent, 16) || field<LONG>(permanent, 8) >= 0)
            return false;
    } else {
        dispatchLiveFailure = 4;
        const auto count = field<size_t>(source, 0x88);
        offers = field<Ptr>(source, 0x80);
        if (!count || count > 100 || !readable(offers, count * 8))
            return false;
        offer = field<Ptr>(offers, 0);
        if (!readable(offer, 0xd8) || field<Ptr>(offer, 0) != engine + 0x2351f18)
            return false;
        dispatchLiveFailure = 5;
        const auto pairs = field<size_t>(cargo, 0xe0);
        const Ptr candidates = field<Ptr>(cargo, 0xd8);
        if (!definition || !pairs || pairs > 256 || !readable(candidates, pairs * 16))
            return false;
        for (size_t i = 0; i < pairs; ++i)
            if (field<Ptr>(candidates, i * 16 + 8) == definition) {
                pair = candidates + i * 16;
                break;
            }
        if (!pair || !readable(field<Ptr>(pair, 0), 16) || !readable(definition, 16) ||
            field<LONG>(definition, 8) >= 0 || field<LONG>(field<Ptr>(pair, 0), 8) >= 0)
            return false;
    }
    // All parameters and unit classes are checked before the engine changes an
    // existing source offer. Engine APIs preserve string and unit reference counts.
    LiveFreightString cargoArg{engine + 0x22217a0, request.cargo + 6,
                               static_cast<unsigned>(strlen(request.cargo + 6)), 128},
        sourceArg{engine + 0x22217a0, request.source, static_cast<unsigned>(strlen(request.source)), 128},
        targetArg{engine + 0x22217a0, request.target, static_cast<unsigned>(strlen(request.target)), 128};
    call<void>(0x4ab6d0, economy, &cargoArg, &sourceArg, &targetArg);
    if (dispatchLiveOwned) {
        dispatchLiveFailure = 7;
        const auto count = field<size_t>(source, 0xb0);
        const Ptr list = field<Ptr>(source, 0xa8);
        if (!count || count > 256 || !readable(list, count * 8))
            return false;
        for (size_t i = 0; i < count; ++i) {
            const Ptr candidate = field<Ptr>(list, i * 8);
            if (!readable(candidate, 0xe0) || field<Ptr>(candidate, 0) != engine + 0x2351f88 ||
                field<Ptr>(candidate, 0x58) != cargo || !liveFreightTargetMatches(candidate, request.target))
                continue;
            const auto amount = field<unsigned>(candidate, 0xcc);
            if (!amount || amount > 10000)
                continue;
            dispatchLiveUnits = amount;
            return acceptLiveDelivery(economy, source, candidate, true);
        }
        return false;
    }
    dispatchLiveFailure = 7;
    if (field<Ptr>(offers, 0) != offer || field<Ptr>(offer, 0x58) != cargo ||
        !liveFreightTargetMatches(offer, request.target))
        return false;
    call<void>(0x3bd2f0, offer + 0x88, pair);
    call<void>(0x3bd2f0, offer + 0x90, pair + 8);
    field<unsigned short>(offer, 0x50) = static_cast<unsigned short>(request.distance);
    field<unsigned>(offer, 0xcc) = request.units;
    field<float>(offer, 0xd0) = 1.f;
    if (field<Ptr>(offer, 0x90) != definition || field<Ptr>(offer, 0x88) != field<Ptr>(pair, 0))
        return false;
    dispatchLiveUnits = request.units;
    return acceptLiveDelivery(economy, source, offer, false);
}
bool guardedLiveFreight(const LiveFreightRequest* request) {
    __try {
        return createLiveFreight(*request);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dispatchLiveFailure = 8;
        return false;
    }
}
void processLiveFreight() {
    LiveFreightRequest request;
    AcquireSRWLockExclusive(&dispatchLiveLock);
    if (!dispatchLiveQueued || dispatchLiveProcessing) {
        ReleaseSRWLockExclusive(&dispatchLiveLock);
        return;
    }
    request = dispatchLivePending;
    dispatchLiveQueued = false;
    dispatchLiveProcessing = true;
    dispatchLiveDone = 0;
    ReleaseSRWLockExclusive(&dispatchLiveLock);
    // Acceptance may pump loading callbacks. Hold no SRW lock across engine calls.
    dispatchLiveFailure = 9;
    const bool okay = GetTickCount64() - request.at < 8000 && guardedLiveFreight(&request);
    AcquireSRWLockExclusive(&dispatchLiveLock);
    dispatchLiveOkay = okay;
    dispatchLiveDone = GetTickCount64();
    dispatchLiveDoneId = request.id;
    dispatchLiveProcessing = false;
    ReleaseSRWLockExclusive(&dispatchLiveLock);
    char line[240]{};
    sprintf_s(line, "[HeavyMP Missions] Direct delivery %s, stage=%u; %s",
              okay ? "acceptance requested" : "refused", dispatchLiveFailure, liveFreightReason());
    log(line);
}
void dispatchLiveEligible(bool value) {
    dispatchLiveEligibleAt = value ? GetTickCount64() : 0;
}
void dispatchLivePacket(const unsigned char* packet, size_t bytes) {
    if (!enabled || faulted || bytes != 544 || memcmp(packet, "HMD1", 4))
        return;
    unsigned version = 0;
    memcpy(&version, packet + 4, 4);
    if (version != 1)
        return;
    LiveFreightRequest r;
    memcpy(&r.units, packet + 8, 4);
    memcpy(&r.distance, packet + 12, 4);
    memcpy(r.id.data(), packet + 16, 16);
    memcpy(r.cargo, packet + 32, 128);
    memcpy(r.source, packet + 160, 128);
    memcpy(r.target, packet + 288, 128);
    memcpy(r.trailer, packet + 416, 128);
    if (!liveFreightToken(r.cargo) || strncmp(r.cargo, "cargo.", 6) || !liveFreightToken(r.source) ||
        !liveFreightToken(r.target) || !liveFreightToken(r.trailer) ||
        strncmp(r.trailer, "trailer_def.", 12) || !r.units || r.units > 10000 || !r.distance ||
        r.distance > 15000)
        return;
    AcquireSRWLockExclusive(&dispatchLiveLock);
    if (!dispatchLiveQueued && !dispatchLiveProcessing && r.id != dispatchLiveDoneId &&
        dispatchLiveFailure != 8) {
        r.at = GetTickCount64();
        dispatchLivePending = r;
        dispatchLiveQueued = true;
    }
    ReleaseSRWLockExclusive(&dispatchLiveLock);
}
bool dispatchLiveResult(char* id, bool* okay, const char** reason, const char** market, unsigned* units) {
    AcquireSRWLockShared(&dispatchLiveLock);
    const bool ready = dispatchLiveDone && GetTickCount64() - dispatchLiveDone < 14000;
    if (ready) {
        unsigned at = 0;
        for (unsigned i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                id[at++] = '-';
            sprintf_s(id + at, 37 - at, "%02x", dispatchLiveDoneId[i]);
            at += 2;
        }
        id[36] = 0;
        *okay = dispatchLiveOkay;
        *reason = liveFreightReason();
        if (market)
            *market = dispatchLiveOwned ? "cargo_market" : "freight_market";
        if (units)
            *units = dispatchLiveUnits;
    }
    ReleaseSRWLockShared(&dispatchLiveLock);
    return ready;
}
