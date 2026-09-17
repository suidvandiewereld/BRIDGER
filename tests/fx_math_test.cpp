
#include "fx/math.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

using namespace bridger::fx::math;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}

Vec4 project(const Mat4& view_proj, float x, float y, float z) {
    return transform(view_proj, {x, y, z, 1.0f});
}

}

int main() {
    const Vec3 right{1.0f, 0.0f, 0.0f};
    const Vec3 forward{0.0f, 1.0f, 0.0f};
    const Vec3 up{0.0f, 0.0f, 1.0f};
    const double position[3] = {100.0, 200.0, 50.0};
    const Mat4 view = view_matrix(right, forward, up, position);

    const Vec4 origin = transform(view, {100.0f, 200.0f, 50.0f, 1.0f});
    check(near(origin.x, 0.0f) && near(origin.y, 0.0f) && near(origin.z, 0.0f), "camera maps to origin");

    const Vec4 ahead = transform(view, {100.0f, 210.0f, 50.0f, 1.0f});
    check(near(ahead.x, 0.0f) && near(ahead.y, 0.0f) && near(ahead.z, -10.0f), "forward is -z");

    const Vec4 above = transform(view, {100.0f, 210.0f, 53.0f, 1.0f});
    check(near(above.y, 3.0f), "up is +y");
    const Vec4 side = transform(view, {104.0f, 210.0f, 50.0f, 1.0f});
    check(near(side.x, 4.0f), "right is +x");

    const float tan_half = std::tan(3.14159265f / 4.0f);
    const float aspect = 16.0f / 9.0f;
    const Mat4 proj = projection_matrix(tan_half, aspect, 0.1f);
    const Mat4 view_proj = multiply(proj, view);
    const auto jittered = jitter_projection(proj, 0.375f, -0.25f, 1920, 1080);
    for (float distance : {1.0f, 10.0f, 1000.0f}) {
        const auto plain = transform(proj, {0.2f, 0.1f, -distance, 1.0f});
        const auto moved = transform(jittered, {0.2f, 0.1f, -distance, 1.0f});
        check(near((moved.x / moved.w - plain.x / plain.w) * 960.0f, 0.375f), "jitter X is in render pixels");
        check(near((plain.y / plain.w - moved.y / moved.w) * 540.0f, -0.25f), "jitter Y is in screen pixels");
        check(near(plain.z / plain.w, moved.z / moved.w), "jitter preserves depth");
    }

    const Vec4 near_point = project(view_proj, 100.0f, 200.1f, 50.0f);
    check(near(near_point.z / near_point.w, 1.0f, 1e-3f), "near plane depth is 1");
    const Vec4 far_point = project(view_proj, 100.0f, 200.0f + 100000.0f, 50.0f);
    check(far_point.z / far_point.w < 1e-3f, "far depth tends to 0");
    check(near(far_point.z / far_point.w, 0.1f / 100000.0f, 1e-6f), "depth is near / distance");

    const Vec4 centre = project(view_proj, 100.0f, 210.0f, 50.0f);
    check(near(centre.x / centre.w, 0.0f) && near(centre.y / centre.w, 0.0f), "centre is ndc origin");
    const Vec4 to_right = project(view_proj, 105.0f, 210.0f, 50.0f);
    check(to_right.x / to_right.w > 0.0f, "right of centre is +x");
    const Vec4 to_up = project(view_proj, 100.0f, 210.0f, 55.0f);
    check(to_up.y / to_up.w > 0.0f, "above centre is +y");

    check(near(to_up.y / to_up.w, 0.5f, 1e-4f), "half the fov is half the ndc range");
    const Vec4 to_edge = project(view_proj, 100.0f + 10.0f * tan_half * aspect, 210.0f, 50.0f);
    check(near(to_edge.x / to_edge.w, 1.0f, 1e-4f), "horizontal edge is ndc 1");

    const Vec4 behind = project(view_proj, 100.0f, 190.0f, 50.0f);
    check(behind.w < 0.0f, "behind the camera has negative w");

    const Mat4 inv = inverse(view_proj);
    const Vec4 clip = project(view_proj, 103.0f, 217.0f, 48.0f);
    const Vec4 back = transform(inv, clip);
    check(near(back.x / back.w, 103.0f, 1e-2f) && near(back.y / back.w, 217.0f, 1e-2f)
              && near(back.z / back.w, 48.0f, 1e-2f), "inverse view-projection round trips");

    const Vec3 r2{0.0f, 1.0f, 0.0f};
    const Vec3 f2{-1.0f, 0.0f, 0.0f};
    const Vec3 u2{0.0f, 0.0f, 1.0f};
    const Mat4 view2 = view_matrix(r2, f2, u2, position);
    const Vec4 ahead2 = transform(view2, {90.0f, 200.0f, 50.0f, 1.0f});
    check(near(ahead2.z, -10.0f) && near(ahead2.x, 0.0f), "rotated basis looks along -x");

    const double far_position[3] = {20000.0, -15000.0, 300.0};
    const Mat4 view3 = view_matrix(right, forward, up, far_position);
    const Vec4 close = transform(view3, {20000.0f, -14990.0f, 300.0f, 1.0f});
    check(near(close.z, -10.0f, 0.05f), "large coordinates keep precision");

    if (g_failures == 0) {
        std::printf("fx_math: all checks passed\n");
        return 0;
    }
    std::printf("fx_math: %d failure(s)\n", g_failures);
    return 1;
}
