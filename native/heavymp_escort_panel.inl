std::atomic<bool> escortBeaconActive{false};
bool localPanelActive = false, localPanelFault = false;
HeavyMpPanelRig localPanelRig;
bool hasEscortPanel(const HeavyMpAppearance* appearance) {
    if (!appearance || appearance->trailer)
        return false;
    for (const auto& item : appearance->accessories)
        if (item.path == "/def/vehicle/truck/heavymp.escort/cabin/escort_3.sii")
            return true;
    return false;
}
void applyEscortPanel(Ptr physics, HeavyMpPanelRig& rig, bool open) {
    if (rig.fault || !wiperReady || !readable(physics, 0xfe8))
        return;
    const Ptr model = field<Ptr>(physics, 0xfe0);
    if (!readable(model, 0x220) || field<Ptr>(model, 0) != engine + 0x2492a70)
        return;
    const auto now = GetTickCount64();
    if (rig.model != model) {
        rig = {};
        rig.model = model;
    }
    if (rig.index < 0) {
        if (rig.retryAt && now - rig.retryAt < 500)
            return;
        rig.retryAt = now;
        const char* path = "/vehicle/heavymp/escort/panel_move.pma";
        const Ptr resource = call<Ptr>(0x160f600, &path);
        if (!resource)
            return;
        const unsigned index = call<unsigned>(0x1611ac0, model, resource, static_cast<unsigned char>(0),
                                              unsigned{0x10}, unsigned{0});
        if (index == 0xffffffffu || index >= 64)
            return;
        rig.index = static_cast<int>(index);
    }
    rig.advance(open, now);
    const unsigned index = static_cast<unsigned>(rig.index);
    if (index >= field<size_t>(model, 0x218))
        return;
    call<void>(0x643650, Ptr{0}, physics + 0xfe0, index, std::min(rig.phase, .9999f));
}
void localEscortPanelFrame() {
    if (!enabled || faulted || localPanelFault) {
        localPanelActive = false;
        return;
    }
    const Ptr world = field<Ptr>(engine, 0x3714fb0);
    if (!readable(world, 0x31b8))
        return;
    const Ptr actor = field<Ptr>(world, 0x31b0);
    if (!readable(actor, 0x20))
        return;
    const Ptr physics = field<Ptr>(actor, 0x18);
    if (!readable(physics, 0xfe8) || field<Ptr>(physics, 0) != engine + 0x2332578)
        return;
    HeavyMpAppearance appearance;
    const bool active =
        readAppearanceDefinition(field<Ptr>(physics, 0x1f8), appearance) && hasEscortPanel(&appearance);
    if (!active) {
        localPanelActive = false;
        localPanelRig = {};
        return;
    }
    localPanelActive = true;
    applyEscortPanel(physics, localPanelRig, escortBeaconActive.load());
}
void guardedLocalEscortPanel() {
    __try {
        localEscortPanelFrame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        localPanelFault = true;
        localPanelActive = false;
        log("[HeavyMP panel] Local sign animation isolated; driving preserved.");
    }
}
void remoteEscortPanel(Truck& truck) {
    if (!truck.attached || truck.trailer || !hasEscortPanel(truck.appearance.get()))
        return;
    applyEscortPanel(truck.physics, truck.panel, (truck.animation.lights & heavyMpPanelLightBit) != 0);
}
void guardedRemoteEscortPanel(Truck& truck) {
    __try {
        remoteEscortPanel(truck);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!truck.panel.fault)
            log("[HeavyMP panel] Remote sign animation isolated; vehicle preserved.");
        truck.panel.fault = true;
    }
}
