// Wide screens see more instead of less (Hor+) in GP4's 3D view.
//
// GP4 builds its camera in 0x493aa0: horizontal FOV h = [0xa532a0] from the camera's zoom,
// then the vertical one from the screen's aspect [0xa532d0]: 0x493ecf..0x493edb is
//   fld1; fpatan; fadd st0, st0; fstp [0xa53290]        ; v = 2 atan(tan(h/2) / aspect)
// The projection matrix AND GP4's own frustum culling (0x446240) are built from those two
// globals right after, so changing them here keeps the image and the culling in step
// (changing only the matrix sent to Direct3D would show holes where GP4 culled).
// The patch replaces those 12 bytes with a call to a stub that stores hor_plus_fov() in both
// globals (core/projection.h). The stub keeps the flags (GP4 branches on them right after),
// all registers, and the value below it on the x87 stack (the near plane).
#pragma once
#include <cstdint>
#include <string>

namespace gp4u {

struct WideFovTargets {
    uint32_t site;     // the 12 bytes above
    uint32_t hfov;     // float, radians
    uint32_t vfov;     // float, radians
    uint32_t aspect;   // float, width / height of GP4's 3D view
};
WideFovTargets gp4_wide_fov_targets();
extern const uint8_t kWideFovSite[12];

// ref_aspect: up to this aspect GP4's view is unchanged (16:9 by default).
// zoom: > 1 brings the camera closer (narrower view), see hor_plus_fov.
bool install_wide_fov(const WideFovTargets& t, double ref_aspect, std::string* why, double zoom = 1.0);

// For the tests: point the stub at other globals without patching code, and call it.
void wide_fov_configure(const WideFovTargets& t, double ref_aspect, double zoom = 1.0);
void* wide_fov_stub();
unsigned wide_fov_calls();

}  // namespace gp4u
