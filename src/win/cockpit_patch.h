// Runtime patch: the cockpit steering wheel turns like the physical one (see core/cockpit.h).
// Writes a 5-byte jmp over GP4.exe 0x409a47 in this process's memory, only after checking
// the original bytes; the game file on disk is never touched.
#pragma once
#include <cstdint>
#include <string>

namespace gp4u {

struct CockpitTargets {   // overridable so the e2e test can patch its own copy
    uint32_t func, mul_a, limit, mul_b;
};
CockpitTargets gp4_cockpit_targets();

// lock_to_lock_deg <= 0: do nothing. Returns false (and why) if not applied.
bool install_cockpit_wheel(double lock_to_lock_deg, const CockpitTargets& t, std::string* why);

}  // namespace gp4u
