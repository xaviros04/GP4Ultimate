// Projection-matrix helpers for wide and multi-screen output (pure C++, unit-tested).
// D3D8 left-handed perspective: _11 = xScale, _22 = yScale, _34 = 1, _44 = 0.
#pragma once

namespace gp4u {

struct Mat4 {
    float m[4][4];   // row-major, m[row][col] == D3DMATRIX::_<row+1><col+1>
};

// True for a standard perspective projection (as built by D3DXMatrixPerspective*LH).
bool is_perspective(const Mat4& p);

// Aspect ratio (width/height) a perspective matrix was built for.
double projection_aspect(const Mat4& p);

// Horizontal-FOV correction ("Hor+"): keeps the vertical FOV and widens the horizontal one
// so the image is not stretched on a wider target. `fov_scale` > 1 zooms out further.
// Returns false (and leaves `p` alone) for non-perspective matrices.
bool fix_aspect(Mat4& p, double target_aspect, double fov_scale = 1.0);

// Horizontal FOV in degrees of a perspective matrix.
double horizontal_fov_deg(const Mat4& p);

// Per-screen projections for N angled monitors (the advanced triple-screen mode): screen i
// of n gets the centre projection's vertical FOV, its own aspect, and a yaw offset so the
// three views join at the bezels. `screen_aspect` is one monitor's width/height,
// `bezel_frac` the bezel width as a fraction of one screen's width.
// Returns the yaw (radians, positive = right) for screen i; `out` gets its projection.
double screen_view(const Mat4& centre, int i, int n, double screen_aspect, double bezel_frac, Mat4& out);

// GP4's camera is "Vert-": it fixes the horizontal FOV `h` and derives the vertical one from
// the screen's aspect, v = 2 atan(tan(h/2) / aspect), so a wider screen sees less above and
// below (0x493aa0, docs/GP4-FINDINGS.md "Cámara"). hor_plus_fov keeps GP4's view up to
// `ref_aspect` and, on wider screens, keeps the vertical FOV GP4 has at `ref_aspect` and
// widens the horizontal one instead. Angles in radians; the horizontal one stays < 179 deg.
struct CameraFov { double h, v; };
// zoom > 1 narrows the view (the camera seems closer), < 1 widens it; it applies at any aspect.
CameraFov hor_plus_fov(double h, double aspect, double ref_aspect, double zoom = 1.0);

}  // namespace gp4u
