// GP4 per-car dynamic data (0x41C bytes per car), as far as the community has mapped it.
//
// Offsets come from GP4PP's Utils/CarDynamicData.h (MIT, Diego 'Oggo' Noriega), which is
// derived from Rene 'SDI' Smit's work with contributions by Paulo 'Prblanco' Blanco.
// GP4PP targets the same GP4.exe build as GP4 Ultimate (sha256 2046f390...4a926b12).
// Fields marked [unverified] are labelled "unclear"/"might be" upstream or have units we
// have not confirmed in-game; everything that uses them is gated behind config options.
#pragma once
#include <cstdint>
#include <cstring>

namespace gp4u {
namespace car {

constexpr uint32_t kSize = 0x41C;

constexpr uint32_t kAngleZ = 0x00C;         // int32 heading (yaw) [unverified units]
constexpr uint32_t kSpeedAngleZ = 0x018;    // uint16 heading of the velocity vector [unverified units]
constexpr uint32_t kSteer = 0x03C;          // int32 steering after steering help
constexpr uint32_t kForceFeedback = 0x05E;  // int16 GP4's force value, +-10000 (verified: written by 0x41a2ec/0x41a463)
constexpr uint32_t kEngineRpm = 0x062;      // uint16
constexpr uint32_t kSteerRaw = 0x06A;       // uint16 raw steering input
constexpr uint32_t kBrakeRaw = 0x06C;       // uint8
constexpr uint32_t kClutchRaw = 0x06D;      // uint8
constexpr uint32_t kThrottleRaw = 0x06E;    // uint8
constexpr uint32_t kCurrentGear = 0x072;    // uint8
constexpr uint32_t kTeamIndex = 0x073;      // uint8
constexpr uint32_t kId = 0x07C;             // uint8, >= 0x80 = player
constexpr uint32_t kTyreCompound = 0x07F;   // uint8
constexpr uint32_t kSpeed = 0x098;          // int32, 0x400000 = 1 ft/s
constexpr uint32_t kSpeedX = 0x0A4;         // int32 world velocity [unverified units]
constexpr uint32_t kSpeedY = 0x0A8;         // int32 world velocity [unverified units]
constexpr uint32_t kWeight = 0x0B2;         // uint16, lbs = 502 + fuel
constexpr uint32_t kFfScaling = 0x173;      // uint8 in-game FF strength, percent (verified: * x / 100 at 0x41a41c)
constexpr uint32_t kNumGears = 0x1CD;       // uint8 forward gears
constexpr uint32_t kMaxLock = 0x1D0;        // uint8 steering lock setting [unverified units]
constexpr uint32_t kRearWing = 0x227;       // uint8
constexpr uint32_t kFrontWing = 0x26A;      // uint8
constexpr uint32_t kTyreLoad = 0x2EC;       // 4 x int32; [2],[3] = front tyres (verified: FF damping weight at 0x41a395)
constexpr uint32_t kSuspTravel = 0x32C;     // 4 x uint32 suspension travel
constexpr uint32_t kWheelSpeed = 0x39C;     // 4 x uint32

constexpr double kFeetPerSecondRaw = 4194304.0;  // 0x400000
constexpr double kFootToMetre = 0.3048;

template <typename T>
inline T rd(const uint8_t* p, uint32_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

// Decoded view of one car record.
struct Sample {
    uint8_t id = 0;
    bool player = false;
    double speed_mps = 0;
    int32_t yaw_raw = 0;
    uint16_t vel_heading_raw = 0;
    int32_t vx_raw = 0, vy_raw = 0;
    int16_t game_ffb = 0;
    int16_t game_align = 0;   // GP4's aligning term alone, scaled like +0x5E (0 if unknown)
    bool has_align = false;   // game_align was read this sample
    // From the per-car wheel record (core/gp4_addresses.h), wheel 0 RL, 1 RR, 2 FL, 3 FR.
    bool has_wheels = false;
    int32_t surface[4] = {}, slide_lat[4] = {}, slide_long[4] = {};
    // From the physics-step hook (win/physics_hook.h): tyre-frame longitudinal force.
    bool has_forces = false;
    int32_t fx[4] = {};
    int32_t g_long = 0;
    uint16_t rpm = 0;
    uint8_t gear = 0, num_gears = 0;
    int32_t steer = 0;
    uint16_t steer_raw = 0;
    uint8_t throttle_raw = 0, brake_raw = 0, clutch_raw = 0;
    uint16_t weight_lbs = 0;
    uint8_t ff_scaling = 0, max_lock = 0;
    int32_t susp[4] = {};
    int32_t tyre_load[4] = {};
    int32_t wheel_speed[4] = {};
};

inline Sample decode(const uint8_t* p) {
    Sample s;
    s.id = rd<uint8_t>(p, kId);
    s.player = (s.id & 0x80) != 0;
    s.speed_mps = rd<int32_t>(p, kSpeed) / kFeetPerSecondRaw * kFootToMetre;
    s.yaw_raw = rd<int32_t>(p, kAngleZ);
    s.vel_heading_raw = rd<uint16_t>(p, kSpeedAngleZ);
    s.vx_raw = rd<int32_t>(p, kSpeedX);
    s.vy_raw = rd<int32_t>(p, kSpeedY);
    s.game_ffb = (int16_t)rd<uint16_t>(p, kForceFeedback);
    s.rpm = rd<uint16_t>(p, kEngineRpm);
    s.gear = rd<uint8_t>(p, kCurrentGear);
    s.num_gears = rd<uint8_t>(p, kNumGears);
    s.steer = rd<int32_t>(p, kSteer);
    s.steer_raw = rd<uint16_t>(p, kSteerRaw);
    s.throttle_raw = rd<uint8_t>(p, kThrottleRaw);
    s.brake_raw = rd<uint8_t>(p, kBrakeRaw);
    s.clutch_raw = rd<uint8_t>(p, kClutchRaw);
    s.weight_lbs = rd<uint16_t>(p, kWeight);
    s.ff_scaling = rd<uint8_t>(p, kFfScaling);
    s.max_lock = rd<uint8_t>(p, kMaxLock);
    for (int i = 0; i < 4; ++i) {
        s.susp[i] = rd<int32_t>(p, kSuspTravel + 4 * i);
        s.tyre_load[i] = rd<int32_t>(p, kTyreLoad + 4 * i);
        s.wheel_speed[i] = rd<int32_t>(p, kWheelSpeed + 4 * i);
    }
    return s;
}

// Cheap structural check: does this record look like a car? Uses only fields whose
// meaning is well established upstream (gear count, weight, team, compound, wings).
// Returns nullptr if plausible, otherwise the first failing check (for the log).
inline const char* implausible_reason(const uint8_t* p) {
    uint8_t ng = rd<uint8_t>(p, kNumGears);
    if (ng < 4 || ng > 7) return "numGears";
    uint16_t w = rd<uint16_t>(p, kWeight);
    // Upstream documents "lbs = 502 + fuel", but the real game (this build, verified in-game
    // on the player car) shows ~1347-1355 falling with fuel, so the unit/offset differs.
    // Keep the range wide: it only has to reject garbage, not validate physics.
    if (w < 480 || w > 2200) return "weight";
    if (rd<uint8_t>(p, kCurrentGear) > 8) return "currentGear";
    if (rd<uint8_t>(p, kTeamIndex) > 15) return "teamIndex";
    if (rd<uint8_t>(p, kTyreCompound) > 15) return "tyreCompound";
    if (rd<uint8_t>(p, kFrontWing) > 40 || rd<uint8_t>(p, kRearWing) > 40) return "wings";
    return nullptr;
}

inline bool plausible(const uint8_t* p) { return implausible_reason(p) == nullptr; }

}  // namespace car
}  // namespace gp4u
