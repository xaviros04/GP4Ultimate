// Snapshot of the player's per-wheel forces at the end of each GP4 physics step.
//
// GP4's wheel block (0x6380D8 + i*0x300, wheel 0 RL 1 RR 2 FL 3 FR) is scratch reused for
// all 22 cars, so another thread cannot read the player's values from it. The call that
// closes each car's step (0x41874A: call 0x418942, car in esi, registers restored by the
// caller's popal right after) is redirected to a stub that makes the call and then, for
// the player's car only, copies the fields below into a seqlock-protected snapshot.
// See docs/GP4-FINDINGS.md "Ruedas".
#pragma once
#include <cstdint>
#include <string>

namespace gp4u {

struct PhysSnap {
    uint32_t steps = 0;            // player steps captured so far
    int32_t fx[4] = {}, fy[4] = {}, mz[4] = {};   // tyre frame Fx (+0x1A0), Fy (+0xE0), Mz (+0x274)
    int32_t g_lat = 0, g_long = 0;               // frame averages (0x623EA0 / 0x623EA8)
};

struct PhysTargets {   // overridable for the e2e test
    uint32_t call_site, step_fn, player_ptr, wheel_block, g_lat, g_long;
};
PhysTargets gp4_phys_targets();

bool install_physics_hook(const PhysTargets& t, std::string* why);
bool physics_hook_installed();
// Consistent copy of the latest snapshot; false until the first player step.
bool physics_snapshot(PhysSnap& out);

}  // namespace gp4u
