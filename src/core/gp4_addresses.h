// Fixed addresses in GP4.exe (sha256 2046f390...4a926b12, image base 0x400000, no
// relocations), established by reading the game's code with the decomp-grandprix4 (gp4re) analysis
// (2026-10-07). Each entry cites the instruction that proves it. Valid only for that build;
// every use is overridable from gp4ultimate.ini.
#pragma once
#include <cstdint>

namespace gp4u {
namespace gp4 {

// Car table: 22 CarDynamicData records of 0x41C bytes.
//   0x409b88  mov esi, 0x66fef0 / mov ecx, 0x16 / ... add esi, 0x41c ; loop  (find player by id)
constexpr uint32_t kCarArray = 0x0066FEF0;
constexpr uint32_t kNumCars = 22;

// Pointer to the player's car record (0 when there is none, e.g. spectating).
//   0x409bec  mov [0x66d820], esi        (set to the record whose id == [0x66d896])
//   0x51e467  mov esi, [0x66d820] ; movsx eax, word [esi+0x5e]   (force feedback source)
constexpr uint32_t kPlayerCarPtr = 0x0066D820;

// GP4's own force feedback.
//   0x41a2ec / 0x41a463 (asm core) compute CarDynamicData+0x5E each physics step:
//     ( aligning + steering-rate damping terms ) * ffScaling(+0x173) / 100, clamped +-10000.
//   0x51e319: value = -clamp(+0x5E, +-10000); 0x5069d0: DI magnitude = clamp(1.5 * value, +-10000)
//   => DirectInput force = -1.5 * (+0x5E) / 10000 of full scale.
constexpr double kGameForceToDI = -1.5 / 10000.0;
// Front-tyre aligning term before damping and scaling (int32), written only for the car
// whose FF is active (flag +0xCF bit 3):
//   0x41a265  [0x603744] = (( -(Fy_w2 + Fy_w3) * k >> 14 ) + Mz_w2 + Mz_w3) * k2 >> 14
//   per-wheel state at 0x6380d8, stride 0x300, wheels 2 and 3 = front.
constexpr uint32_t kAligningTerm = 0x00603744;

// Per-car wheel/effects record, written once per physics step for every car by 0x418942
// (record = 0x418901: 0x625ABC + byte[car+0x202] * 0xA8). Per wheel, int32 x4 (wheel order
// 0 RL, 1 RR, 2 FL, 3 FR - left/right inferred from the wheel positions, 0x4121C7):
//   +0x38 load (= car+0x2EC), +0x48 lateral slide intensity, +0x58 longitudinal slide
//   intensity (0x418a5e/0x418aa9), +0x68 surface under the wheel (from wheel block +0x2D:
//   0 asphalt, 1 kerb, 3 grass, 4 gravel; 0x418a13), +0x78 wheel failure.
// Unlike the shared wheel scratch at 0x6380D8 (reused for all 22 cars), this one stays
// valid between steps, so the FFB thread can read the player's at any time.
constexpr uint32_t kWheelRecord = 0x00625ABC;
constexpr uint32_t kWheelRecordStride = 0xA8;
constexpr uint32_t kCarSlot = 0x202;   // byte: index of the car in the 22-car table
constexpr uint32_t kWrLoad = 0x38, kWrSlideLat = 0x48, kWrSlideLong = 0x58, kWrSurface = 0x68;
enum Surface { kAsphalt = 0, kKerb = 1, kGrass = 3, kGravel = 4 };

// GP4 sends zero force while any of these bytes is non-zero (0x51e405).
constexpr uint32_t kForceZeroFlags[3] = {0x00852D6A, 0x0088F63A, 0x00852D98};

}  // namespace gp4
}  // namespace gp4u
