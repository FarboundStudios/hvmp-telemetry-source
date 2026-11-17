// ETS2 1.62.3.3s: exterior wiper model is tractor physics + 0xfe0.
// Actor 0x5e5b60 binds the cabin's original PMA; 0x643650 seeks its cycle.
bool wiperReady = false;
bool readLocalWipers(float* phase, float* rear, unsigned* mode) {
    if (!phase || !rear || !mode || !enabled || faulted)
        return false;
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8))
        return false;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x26c))
        return false;
    const Ptr physics = field<Ptr>(actor, 0x18);
    if (!readable(physics, 0xfe8) || field<Ptr>(physics, 0) != engine + 0x2332578 ||
        field<int>(actor, 0x224) < 0)
        return false;
    const float p = field<float>(actor, 0x264), r = field<float>(actor, 0x268);
    const unsigned m = field<unsigned>(actor, 0x230);
    if (!std::isfinite(p) || p < 0 || p > 1 || !std::isfinite(r) || r < 0 || r > 1 || m > 16)
        return false;
    *phase = p;
    *rear = r;
    *mode = m;
    return true;
}
bool guardedLocalWipers(float* p, float* r, unsigned* m) {
    __try {
        return readLocalWipers(p, r, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void updateWipers(Truck& t) {
    if (!wiperReady || t.trailer || t.wiperFault || !t.attached || !t.animation.haveWiperPhase ||
        !readable(t.physics, 0x1058))
        return;
    const Ptr model = field<Ptr>(t.physics, 0xfe0);
    if (!readable(model, 0x220) || field<Ptr>(model, 0) != engine + 0x2492a70)
        return;
    if (t.wiperModel != model) {
        t.wiperModel = model;
        t.wiperIndices = {-1, -1};
        t.wiperBound = false;
        t.wiperAt = 0;
    }
    if (!t.wiperBound) {
        const auto now = GetTickCount64();
        if (t.wiperAt && now - t.wiperAt < 500)
            return;
        t.wiperAt = now;
        const Ptr cabin = call<Ptr>(0x8e2f20, field<Ptr>(t.physics, 0x1050));
        if (!readable(cabin, 0x3b0))
            return;
        const Ptr unit = field<Ptr>(t.physics, 0x1f8);
        if (!readable(unit, 0x40))
            return;
        const Ptr interior = call<Ptr>(0x65c1f0, engine + 0x1e52210, field<Ptr>(unit, 0x38));
        const bool uk = readable(interior, 0x1113) && field<unsigned char>(interior, 0x1112) != 0;
        const unsigned offsets[] = {uk ? 0x3a8u : 0x288u, 0x2a8u};
        for (unsigned i = 0; i < 2; ++i) {
            if (t.wiperIndices[i] >= 0)
                continue;
            const Ptr path = cabin + offsets[i], text = field<Ptr>(path, 0);
            if (!readable(text, 1) || !field<char>(text, 0))
                continue;
            const Ptr resource = call<Ptr>(0x160f600, path);
            if (!resource)
                return;
            const unsigned index = call<unsigned>(0x1611ac0, model, resource, static_cast<unsigned char>(0),
                                                  unsigned{0x10}, unsigned{0});
            if (index == 0xffffffffu || index >= 64)
                return;
            t.wiperIndices[i] = static_cast<int>(index);
        }
        t.wiperBound = true;
    }
    const float phases[] = {t.animation.wiperPhase, t.animation.rearWiperPhase};
    for (unsigned i = 0; i < 2; ++i)
        if (t.wiperIndices[i] >= 0) {
            const unsigned index = static_cast<unsigned>(t.wiperIndices[i]);
            if (index >= field<size_t>(model, 0x218))
                continue;
            // RCX is unused, RDX addresses the model pointer, R8=index, XMM3=phase.
            call<void>(0x643650, Ptr{0}, t.physics + 0xfe0, index, phases[i]);
            if (!t.wiperLogged) {
                t.wiperLogged = true;
                log("[HeavyMP wipers] Original exterior PMA bound; remote cycle position applied.");
            }
        }
}
void guardedWipers(Truck& t) {
    __try {
        updateWipers(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t.wiperFault = true;
        log("[HeavyMP wipers] Animation fault isolated; vehicle drawing preserved.");
    }
}
bool validateWipers() {
    return field<unsigned>(engine, 0x643650) == 0x245c8948u &&
           field<unsigned>(engine, 0x160f600) == 0x38ec8348u &&
           field<unsigned>(engine, 0x1611ac0) == 0x245c8948u;
}
