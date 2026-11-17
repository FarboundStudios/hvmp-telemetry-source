// ETS2 5dace0 locates this native shadow-only variant; 798720 draws it
// through 15e0ed0 in render-pass bit 60, separately from colour geometry.
constexpr std::uint64_t shadowPass = 0x1000000000000000ULL;
// The engine's solar mask includes bit 59; bit 60 is the separate shadow-only variant.
constexpr std::uint64_t sunShadowPasses = 0x080000000001fc00ULL;
bool shadowReady = false;
std::atomic<bool> shadowFault{false};
unsigned shadowVariant(Ptr model) {
    if (!readable(model, 0x380))
        return ~0u;
    const Ptr holder = field<Ptr>(model, 0x120);
    if (!readable(holder, 0x20))
        return ~0u;
    const Ptr data = field<Ptr>(holder, 0x18);
    if (!readable(data, 0x58))
        return ~0u;
    const unsigned count = field<unsigned>(data, 8), offset = field<unsigned>(data, 0x4c);
    if (!count || count > 256 || offset > 64 * 1024 * 1024 ||
        !readable(data + offset, static_cast<size_t>(count) * 24))
        return ~0u;
    const auto draws = field<size_t>(model, 0x300);
    for (unsigned i = 0; i < count; ++i) {
        const Ptr record = data + offset + i * 24;
        if (field<std::uint64_t>(record, 0) != 0x1a7bec2cafe263ULL)
            continue;
        const unsigned number = field<unsigned>(record, 8), start = field<unsigned>(record, 12);
        if (number && number <= 4096 && draws <= 65536 && start < draws && number <= draws - start)
            return i;
    }
    return ~0u;
}
bool nativeShadow(Truck& t, Ptr context) {
    const auto count = field<size_t>(t.physics, 0x168);
    const Ptr array = field<Ptr>(t.physics, 0x160);
    if (!count || count > 256 || !readable(array, count * 8))
        return false;
    const Ptr model = field<Ptr>(array, 0);
    const unsigned variant = shadowVariant(model);
    if (variant == ~0u)
        return false;
    call<void>(0x16166b0, model);
    call<void>(0x16166f0, model, context, variant);
    if (!t.shadowCalls)
        log(t.trailer ? "[HeavyMP shadow] Native trailer shadow-only variant submitted; visual verification "
                        "required."
                      : "[HeavyMP shadow] Native tractor shadow-only variant submitted; visual verification "
                        "required.");
    ++t.shadowCalls;
    return true;
}
bool guardedShadow(Truck& t, Ptr context) {
    __try {
        return nativeShadow(t, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shadowFault = true;
        log("[HeavyMP shadow] Shadow update fault; shadow sync disabled, vehicle drawing preserved.");
        return false;
    }
}
using WorldDraw = void (*)(Ptr, Ptr);
WorldDraw originalWorldDraw[2]{};
void* worldDrawAddresses[2]{};
thread_local Ptr shadowContext = 0;
thread_local bool shadowSubmitted = false;
