#include "heavymp_player_projection.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main() {
    HeavyMpTagCamera c;
    c.right[0] = 1;
    c.up[1] = 1;
    c.forward[2] = -1;
    double x = 0, y = 0;
    double ahead[] = {0, 0, -20};
    assert(heavymp_project_tag(c, ahead, 16. / 9., x, y) && fabs(x - .5) < 1e-8 && fabs(y - .5) < 1e-8);
    double above[] = {0, 3, -20};
    assert(heavymp_project_tag(c, above, 16. / 9., x, y) && y < .5);
    double behind[] = {0, 0, 20};
    assert(!heavymp_project_tag(c, behind, 16. / 9., x, y));
    double outside[] = {1000, 0, -20};
    assert(!heavymp_project_tag(c, outside, 16. / 9., x, y));
    ahead[0] = std::numeric_limits<double>::quiet_NaN();
    assert(!heavymp_project_tag(c, ahead, 16. / 9., x, y));
    // Exact row-major camera projection captured in ETS2's exterior view.
    c.nativeProjection = true;
    c.projection[0] = 1.248209f;
    c.projection[5] = 2.219038f;
    c.projection[10] = -1.00025f;
    c.projection[11] = -.40005f;
    c.projection[14] = -1.f;
    double target[] = {5, 2, -20};
    assert(heavymp_project_tag(c, target, 16. / 9., x, y));
    assert(fabs(x - (.5 + .5 * 5 * 1.248209 / 20)) < 1e-6 && fabs(y - (.5 - .5 * 2 * 2.219038 / 20)) < 1e-6);
    // An external camera moved behind and turned toward +Z must see that side,
    // instead of retaining the interior head's origin and forward direction.
    c.eye[2] = -40;
    c.right[0] = -1;
    c.forward[2] = 1;
    assert(heavymp_project_tag(c, target, 16. / 9., x, y) && x < .5);
    c.projection[2] = .2f;
    assert(heavymp_project_tag(c, target, 16. / 9., x, y) &&
           fabs(x - (.5 - .5 * 5 * 1.248209 / 20 - .1)) < 1e-6);
    c.projection[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!heavymp_project_tag(c, target, 16. / 9., x, y));
    puts("PASS: name-tag projection, above roof, behind camera, outside screen, invalid input.");
    puts("PASS: native exterior projection, moved/rotated camera, asymmetric FOV and invalid matrix.");
}
