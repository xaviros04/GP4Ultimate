// Fix for a GP4 crash in its sound code (seen in long online races).
//
// GP4's sound object (vtable 0x5f9c80) keeps its DirectSound buffers in a table at
// +0x25c. A multimedia-timer thread updates their volumes (0x5a8810 → SetVolume 0x5ad730)
// while the game thread frees buffers (0x5ad9f0: Release until the count is 0, then the
// slot = 0). The two are meant to exclude each other with a busy flag at +0x3c, but:
//   - the flag is set by a plain check-then-write (0x5a85a0 / 0x5a85e0), so both threads
//     can get it at once;
//   - when the game thread fails to get it, it clears it anyway (0x5adb07), releasing the
//     timer thread's hold;
//   - SetVolume reads the slot twice; if it was freed in between it calls through a null
//     pointer (crash report of 10-10-2026: 0x5ad79a, reading address 0).
// The fix replaces three of the object's virtual functions: the busy flag becomes an
// atomic one that waits for its holder (and still fails at once on recursion, as GP4
// expects), and SetVolume re-checks the slot before using it.
#pragma once
#include <cstdint>
#include <string>

namespace gp4u {

struct SoundFixTargets {
    uint32_t vtable;          // the sound class's vtable
    uint32_t vtable2_setvol;  // the same SetVolume in a second vtable (0 = none)
    uint32_t lock_fn;         // vtable +0x0c: try to take the busy flag (0 ok, 5 busy)
    uint32_t set_fn;          // vtable +0x10: take the busy flag
    uint32_t unlock_fn;       // vtable +0x14: clear the busy flag
    uint32_t setvol_fn;       // vtable +0x9c: SetVolume(slot, volume)
};
SoundFixTargets gp4_sound_fix_targets();

bool install_sound_fix(const SoundFixTargets& t, std::string* why);

}  // namespace gp4u
