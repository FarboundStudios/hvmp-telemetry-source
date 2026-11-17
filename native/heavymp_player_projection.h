#pragma once
#include <cmath>
struct HeavyMpTagCamera {
    double eye[3]{}, right[3]{}, up[3]{}, forward[3]{};
    float horizontalFov = 75;
    float projection[16]{};
    bool nativeProjection = false;
};
inline bool heavymp_project_tag(const HeavyMpTagCamera& camera, const double point[3], double aspect,
                                double& x, double& y) {
    if (!std::isfinite(aspect) || aspect <= 0 || !std::isfinite(camera.horizontalFov) ||
        camera.horizontalFov < 30 || camera.horizontalFov > 140)
        return false;
    double sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < 3; ++i) {
        const double v = point[i] - camera.eye[i];
        if (!std::isfinite(v))
            return false;
        sx += v * camera.right[i];
        sy += v * camera.up[i];
        sz += v * camera.forward[i];
    }
    if (!std::isfinite(sz) || sz <= .2)
        return false;
    if (camera.nativeProjection) {
        // ETS2 stores a row-major projection operating on camera-space -Z.
        // Preserve the actual aspect, zoom and asymmetric projection offsets.
        const double v[] = {sx, sy, -sz, 1};
        double clip[4]{};
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) {
                const auto p = camera.projection[row * 4 + col];
                if (!std::isfinite(p))
                    return false;
                clip[row] += p * v[col];
            }
        if (!std::isfinite(clip[3]) || clip[3] <= .001)
            return false;
        x = .5 + .5 * clip[0] / clip[3];
        y = .5 - .5 * clip[1] / clip[3];
        return std::isfinite(x) && std::isfinite(y) && x >= 0 && x <= 1 && y >= 0 && y <= 1;
    }
    const double scale = 1 / std::tan(camera.horizontalFov * 3.14159265358979323846 / 360.0);
    x = .5 + .5 * sx * scale / sz;
    y = .5 - .5 * sy * scale * aspect / sz;
    return std::isfinite(x) && std::isfinite(y) && x >= 0 && x <= 1 && y >= 0 && y <= 1;
}
