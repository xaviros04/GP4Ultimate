#include "projection.h"

#include <cmath>

namespace gp4u {

bool is_perspective(const Mat4& p) {
    return std::fabs(p.m[2][3] - 1.0f) < 1e-4f && std::fabs(p.m[3][3]) < 1e-6f && p.m[0][0] > 0 && p.m[1][1] > 0 &&
           std::fabs(p.m[0][1]) < 1e-6f && std::fabs(p.m[1][0]) < 1e-6f;
}

double projection_aspect(const Mat4& p) {
    return p.m[0][0] > 0 ? (double)p.m[1][1] / (double)p.m[0][0] : 0.0;
}

bool fix_aspect(Mat4& p, double target_aspect, double fov_scale) {
    if (!is_perspective(p) || target_aspect <= 0 || fov_scale <= 0) return false;
    double y = p.m[1][1] / fov_scale;
    p.m[1][1] = (float)y;
    p.m[0][0] = (float)(y / target_aspect);
    return true;
}

double horizontal_fov_deg(const Mat4& p) {
    if (p.m[0][0] <= 0) return 0;
    return 2.0 * std::atan(1.0 / p.m[0][0]) * 180.0 / 3.14159265358979323846;
}

double screen_view(const Mat4& centre, int i, int n, double screen_aspect, double bezel_frac, Mat4& out) {
    out = centre;
    // Each screen keeps the centre's vertical scale; its horizontal half-extent at unit
    // depth is tan(hfov/2) = screen_aspect / yScale. Neighbouring screens are rotated by the
    // angle subtended by one screen plus one bezel at the eye.
    double y = centre.m[1][1];
    out.m[0][0] = (float)(y / screen_aspect);
    double half = std::atan(screen_aspect / y);           // half horizontal FOV of one screen
    double bezel = std::atan(screen_aspect * bezel_frac / y);
    double step = 2.0 * half + bezel;
    double k = i - (n - 1) / 2.0;                          // -1, 0, +1 for three screens
    return k * step;
}

CameraFov hor_plus_fov(double h, double aspect, double ref_aspect, double zoom) {
    if (!(aspect > 0)) return {h, h};
    double t = std::tan(h / 2.0);
    if (zoom > 0 && zoom != 1.0) {
        t /= zoom;
        h = 2.0 * std::atan(t);
    }
    if (!(ref_aspect > 0) || aspect <= ref_aspect) return {h, 2.0 * std::atan(t / aspect)};
    double v = 2.0 * std::atan(t / ref_aspect);
    double wide = 2.0 * std::atan(t * aspect / ref_aspect);
    const double kMax = 179.0 * 3.14159265358979323846 / 180.0;
    return {wide < kMax ? wide : kMax, v};
}

}  // namespace gp4u
