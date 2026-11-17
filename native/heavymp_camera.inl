// camera_manager_u at engine+0x3715018 selects the displayed camera. Its
// native accessor 0x50d1b0 returns selected camera +0x40 (sector placement).
// +0x60 is the exact projection used by that camera, verified in the running
// ETS2 1.62.3.3s external view; no SDK head offset or configured FOV is used.
bool nativeCameraSnapshot(HeavyMpTagCamera* output) {
    if (!output || !enabled || faulted)
        return false;
    const Ptr manager = field<Ptr>(engine, 0x3715018);
    if (!readable(manager, 0x78))
        return false;
    Ptr selected = 0;
    const auto stack = field<std::size_t>(manager, 0x70);
    if (stack) {
        const Ptr array = field<Ptr>(manager, 0x68);
        if (stack > 32 || !readable(array, stack * 8))
            return false;
        selected = field<Ptr>(array, (stack - 1) * 8);
    } else {
        const auto index = field<unsigned>(manager, 0x10);
        const auto count = field<std::size_t>(manager, 0x40);
        const Ptr array = field<Ptr>(manager, 0x38);
        if (!count || count > 64 || index >= count || !readable(array, count * 8))
            return false;
        selected = field<Ptr>(array, index * 8);
    }
    if (!readable(selected, 0xa0))
        return false;
    const Placement pose = field<Placement>(selected, 0x40);
    const double norm = pose.w * pose.w + pose.qx * pose.qx + pose.qy * pose.qy + pose.qz * pose.qz;
    if (!std::isfinite(norm) || std::abs(norm - 1.) > .02)
        return false;
    HeavyMpTagCamera result{};
    result.eye[0] = pose.x + pose.sx * 512.;
    result.eye[1] = pose.y;
    result.eye[2] = pose.z + pose.sz * 512.;
    for (double coordinate : result.eye)
        if (!std::isfinite(coordinate) || std::abs(coordinate) > 1e8)
            return false;
    const double w = pose.w, x = pose.qx, y = pose.qy, z = pose.qz;
    const double right[] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
    const double up[] = {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
    const double forward[] = {-2 * (x * z + w * y), -2 * (y * z - w * x), -(1 - 2 * (x * x + y * y))};
    memcpy(result.right, right, sizeof(right));
    memcpy(result.up, up, sizeof(up));
    memcpy(result.forward, forward, sizeof(forward));
    memcpy(result.projection, reinterpret_cast<const void*>(selected + 0x60), sizeof(result.projection));
    for (float value : result.projection)
        if (!std::isfinite(value) || std::abs(value) > 1e6)
            return false;
    if (result.projection[0] <= 0 || result.projection[5] <= 0 ||
        std::abs(result.projection[14] + 1.f) > .01f || std::abs(result.projection[15]) > .01f)
        return false;
    result.nativeProjection = true;
    *output = result;
    return true;
}
bool guardedCameraSnapshot(HeavyMpTagCamera* output) {
    __try {
        return nativeCameraSnapshot(output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
