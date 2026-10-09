// Cockpit steering-wheel model rotation (GP4.exe 0x409a47), as pure functions.
//
// GP4 turns the 3D wheel in the cockpit by a fixed multiple of the FRONT-WHEEL angle
// (car +0x418, smoothed, 65536 = full turn): x1.5 (unless car +0xE5 bit 0), clamped to
// +-[0x6013ac] (= 20 degrees), x7.0, x[0x603468]/16384 (= 1.125). So the visual wheel
// reaches at most 157.5 degrees and, with a real steering lock below 13.3 degrees, much
// less: it never matches a 360-1080 degree physical wheel. Callers: 0x510882 (stores
// eax & 0xFFFF to 0x8660b0 -> render object +0x4A28 -> CGP4Cockpit +0x44 -> roll of
// cockpit_wheel.gp4) and 0x51b025 (stores ax to 0x876f1e). Evidence: docs/GP4-FINDINGS.md.
//
// The mod's replacement drives it from the raw steering input instead (car +0x6A, int16,
// centred, +-4064 at the extremes calibrated in GP4's controller menu, written at
// 0x51df99; replays write it for the player's car; AI cars and keyboard leave it 0), so
// the visual wheel turns exactly like the physical one. Cars with raw == 0 keep the
// original formula, reproduced exactly below.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gp4u {
namespace cockpit {

constexpr uint32_t kFunc = 0x409a47;
constexpr uint32_t kMulA = 0x603568;     // int32, Q14 (24576 = x1.5)
constexpr uint32_t kLimit = 0x6013ac;    // int32, angle units (3641 = 20 deg), set at 0x411825
constexpr uint32_t kMulB = 0x603468;     // int32, Q14 (18432 = x1.125)
constexpr uint32_t kCarFront = 0x418;    // int16 front-wheel angle (smoothed)
constexpr uint32_t kCarFlags = 0xE5;     // bit 0: skip the x1.5
constexpr uint32_t kCarRaw = 0x06A;      // int16 raw steering input
constexpr int kRawFull = 0x3F80 >> 2;    // 4064: clamp + sar 2 at 0x51df7a..0x51df96

// First bytes of the original function (push edx; movsx eax,[esi+418h]; test byte
// [esi+0E5h],1; jnz; imul [603568h]): checked before patching.
constexpr uint8_t kSignature[] = {0x52, 0x0F, 0xBF, 0x86, 0x18, 0x04, 0x00, 0x00, 0xF6, 0x86, 0xE5, 0x00,
                                  0x00, 0x00, 0x01, 0x75, 0x0A, 0xF7, 0x2D, 0x68, 0x35, 0x60, 0x00};

struct Consts {
    int32_t mul_a = 24576, limit = 3641, mul_b = 18432;
};

// imul r/m32 then shrd eax, edx, 14: low 32 bits of the 64-bit product shifted right by 14.
inline int32_t q14(int32_t a, int32_t b) { return (int32_t)(uint32_t)((uint64_t)((int64_t)a * b) >> 14); }

// Exact re-implementation of 0x409a47 (the x87 step is s * 7.0, exact for |s| <= limit).
inline int32_t original_angle(const uint8_t* car, const Consts& k) {
    int16_t f;
    std::memcpy(&f, car + kCarFront, 2);
    int32_t s = f;
    if (!(car[kCarFlags] & 1)) s = q14(s, k.mul_a);
    uint32_t lim = (uint32_t)k.limit;
    if (s < 0) {
        uint32_t m = 0u - (uint32_t)s;
        if (m >= lim) m = lim;   // jb (unsigned)
        s = (int32_t)(0u - m);
    } else if ((uint32_t)s >= lim) {
        s = (int32_t)lim;
    }
    s *= 7;
    return q14(s, k.mul_b);
}

// Visual angle (65536 = full turn) for a raw input, so that the visual wheel turns
// lock_to_lock_deg in total between the calibrated extremes.
inline int32_t raw_angle(int16_t raw, double lock_to_lock_deg) {
    double units = (double)raw / kRawFull * (lock_to_lock_deg * 0.5) * (65536.0 / 360.0);
    return (int32_t)std::lround(units);
}

inline int32_t patched_angle(const uint8_t* car, const Consts& k, double lock_to_lock_deg) {
    int16_t raw;
    std::memcpy(&raw, car + kCarRaw, 2);
    if (raw != 0) return raw_angle(raw, lock_to_lock_deg);
    return original_angle(car, k);
}

// Degrees shown for a 65536-per-turn angle after the caller's & 0xFFFF (signed view).
inline double visual_degrees(int32_t units) {
    int16_t v = (int16_t)(uint16_t)(units & 0xFFFF);
    return v * 360.0 / 65536.0;
}

}  // namespace cockpit
}  // namespace gp4u
