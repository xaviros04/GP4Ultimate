// Telemetry-driven force-feedback model. Pure C++ so it can be tested and tuned offline
// (e.g. replaying a telemetry CSV); the Windows engine calls step() at a fixed rate
// (500 Hz by default) and sends the result to the wheel as a constant force.
//
// total  = game signal (GP4's own FF value, upsampled) + aligning-torque model (own, off by
//          default) + road texture + shift kick + impacts + damping + friction (+ spring)
// output = -master_gain * softclip(total), then `invert`, min_force, clamp and slew.
//
// Sign convention (verified in-game on a Simucube 2, 2026-10-07):
//  - every component in FfbComponents is PHYSICAL: positive = pushes the wheel towards +X
//    (to the right), so a centring force is negative for steer > 0 and damping is
//    -k * steer_rate;
//  - step() returns a DirectInput constant-force magnitude, and in DirectInput a positive
//    magnitude pushes towards -X (the direction is where the force comes FROM). GP4 itself
//    relies on this: its -1.5 * (+0x5E) is a centring force with that convention. Hence
//    output = -total. `invert` is only for a wheel/driver that does the opposite.
// The first in-game builds mixed both conventions: GP4's force was right but the mod's own
// damping/friction/spring pushed WITH the wheel (it swung lock to lock in the pits and
// ping-ponged around the centre on straights).
inline constexpr double kDirectInputSign = -1.0;
#pragma once
#include <cstdint>

#include "ini.h"

namespace gp4u {

struct FfbParams {
    double master_gain = 0.55;
    bool invert = false;
    double max_output = 1.0;

    // GP4's own per-car force-feedback value (CarDynamicData +0x5E).
    double game_gain = 1.0;
    // Defaults reproduce GP4 exactly: DI force = -1.5 * (+0x5E) / 10000 (verified in
    // GP4.exe at 0x51e319/0x5069d0, see core/gp4_addresses.h).
    double game_scale = 1.5 / 10000.0;
    bool game_invert = true;
    double game_lowpass_hz = 30.0;       // smooths the sim-rate steps at 500 Hz

    // Own self-aligning-torque estimate (bicycle model + simplified Pacejka with pneumatic
    // trail collapsing past the peak). It follows the real wheel at 500 Hz, so it leads
    // GP4's own aligning term (updated at the frame rate) by 20-40 ms. Calibrated on a
    // real race (2026-10-08, 60 fps, 21k samples): corr 0.96 with GP4's aligning term
    // (0.98 above 65 m/s) with steer_lock_deg 12, alpha_peak_deg 6, aero_load 0.0003,
    // GP4 term = 1.30 x model_raw.
    //  model_mix: fraction of GP4's aligning term (0x603744) replaced by the model; GP4's
    //    own damping terms stay. 0 = pure GP4. Needs the known build (aligning address).
    //  model_gain: model added on top of everything (legacy/experiments).
    double model_mix = 0.0;
    double model_scale = 1.3;
    double model_gain = 0.0;
    double yaw_units_per_turn = 4294967296.0;   // CarDynamicData +0x0C [unverified]
    double heading_units_per_turn = 65536.0;    // CarDynamicData +0x18 [unverified]
    double steer_lock_deg = 12.0;               // road-wheel angle at full steering input
    double cg_to_front_m = 1.6;
    double alpha_peak_deg = 6.0;                // front slip angle at peak lateral force
    double aero_load = 0.0003;                  // extra load per (m/s)^2
    double yaw_rate_lowpass_hz = 10.0;

    // Conditioning, from the wheel's own movement.
    double wheel_range_deg = 360.0;             // lock-to-lock rotation of the physical wheel
    // GP4's total signal already contains its own steering damping, so ours is light by
    // default; raise it when using game_signal = aligning (pure aligning term).
    // GP4 updates its force at ~29 Hz and, above ~180 km/h, it reaches full force within
    // ~10 degrees of wheel rotation: a stiff spring that arrives late, which makes a direct
    // drive ping-pong around the centre. Damping that grows with speed holds it; a loop
    // simulation (wheel inertia 0.03-0.1 kg m^2, 6-30 Nm) needs ~0.15-0.2 there.
    double damping = 0.0;                       // per rad/s of wheel speed, always (default: the base does it)
    double damping_speed = 0.15;                // extra, ramping in linearly up to 50 m/s
    // The needed damping scales with GP4's update interval (simulation: 0.15-0.2 at 30 Hz,
    // 0.10-0.12 at 60 Hz). With damping_adapt the damping_speed term is scaled by the
    // measured interval / (1/30 s), clamped to 0.55..1.3, so 30 and 60 fps both feel right.
    bool damping_adapt = true;
    double damping_low_speed = 0.15;            // extra damping fading out by 15 m/s (pit lane, grid)
    double friction = 0.01;
    double no_car_centering = 0.15;             // gentle spring when there is no telemetry

    // Effects.
    // Kerbs/bumps: high-passed left-right difference of front suspension travel. Scale from
    // a real lap: normal track ~0.01-0.03, kerbs 0.1-0.35 before road_gain.
    double road_gain = 3.0;
    double road_scale = 1.0 / 16384.0;
    double road_hp_hz = 6.0;
    int front_left = 2, front_right = 3;        // WheelsD indices 2,3 = front (verified for tyre load)
    // Surface and grip textures from GP4's per-wheel record (ideas from the GP2 FFB mod,
    // github.com/gplaps/GP2FFB, MIT): a speed-dependent vibration on kerbs, rumble on
    // gravel/grass, lighter steering with the front wheels off the asphalt, and a fine
    // scrub when the front tyres slide (understeer warning).
    double kerb_gain = 0.2;
    double gravel_gain = 0.10;
    double grass_gain = 0.04;
    double offroad_light = 0.3;                 // GP4 force reduction with both fronts off asphalt
    double scrub_gain = 0.1;
    double scrub_full = 8192;                   // front slide intensity giving full scrub
    // Braking (GP2FFB's idea, with GP4's own tyre forces): the difference between the front
    // wheels' longitudinal forces turns the wheel through the scrub radius, and a front
    // wheel locking judders. Fx comes from the physics-step hook.
    double brake_gain = 0.15;
    double brake_full = 100000;                 // Fx difference (lbf x 256, ~1700 N) for full effect
    double lockup_gain = 0.10;
    double lockup_full = 8192;                  // front longitudinal slide for full judder
    double shift_kick = 0.12;
    double shift_ms = 50.0;
    double impact_gain = 0.35;
    double impact_g = 2.5;                      // deceleration that counts as an impact

    // Output stage.
    double min_force = 0.0;                     // wheel deadband compensation (0..0.2)
    double slew_per_s = 40.0;                   // max output change per second

    void load(const Ini& ini);                  // section [ffb]

    // Every [ffb] key with its member: the single list load() and the configurator's
    // schema test use, so a new option cannot be read without being described.
    template <class F>
    void for_each(F&& f) {
        f("master_gain", master_gain); f("invert", invert); f("max_output", max_output);
        f("game_gain", game_gain); f("game_scale", game_scale); f("game_invert", game_invert);
        f("game_lowpass_hz", game_lowpass_hz);
        f("model_mix", model_mix); f("model_scale", model_scale);
        f("model_gain", model_gain); f("yaw_units_per_turn", yaw_units_per_turn);
        f("heading_units_per_turn", heading_units_per_turn); f("steer_lock_deg", steer_lock_deg);
        f("cg_to_front_m", cg_to_front_m); f("alpha_peak_deg", alpha_peak_deg); f("aero_load", aero_load);
        f("yaw_rate_lowpass_hz", yaw_rate_lowpass_hz);
        f("wheel_range_deg", wheel_range_deg); f("damping", damping); f("damping_speed", damping_speed);
        f("damping_adapt", damping_adapt);
        f("damping_low_speed", damping_low_speed); f("friction", friction); f("no_car_centering", no_car_centering);
        f("road_gain", road_gain); f("road_scale", road_scale); f("road_hp_hz", road_hp_hz);
        f("front_left", front_left); f("front_right", front_right);
        f("kerb_gain", kerb_gain); f("gravel_gain", gravel_gain); f("grass_gain", grass_gain);
        f("offroad_light", offroad_light); f("scrub_gain", scrub_gain); f("scrub_full", scrub_full);
        f("brake_gain", brake_gain); f("brake_full", brake_full); f("lockup_gain", lockup_gain);
        f("lockup_full", lockup_full);
        f("shift_kick", shift_kick); f("shift_ms", shift_ms);
        f("impact_gain", impact_gain); f("impact_g", impact_g);
        f("min_force", min_force); f("slew_per_s", slew_per_s);
    }
};

struct FfbInput {
    bool car_valid = false;
    double speed_mps = 0;
    int32_t yaw_raw = 0;
    uint16_t vel_heading_raw = 0;
    int16_t game_ffb = 0;
    int16_t game_align = 0;   // GP4's aligning term alone (CSV, model_mix)
    bool has_align = false;
    int gear = 0;
    int32_t susp[4] = {};
    double steer = 0;   // physical wheel position, -1..1
    // Per-wheel state (wheel 0 RL, 1 RR, 2 FL, 3 FR): surface 0 asphalt 1 kerb 3 grass
    // 4 gravel; lateral slide intensity (GP4 units, ~16384 = heavy sliding).
    bool has_wheels = false;
    int surface[4] = {};
    int32_t slide_lat[4] = {};
    int32_t slide_long[4] = {};   // longitudinal slide intensity (front: locking under braking)
    // From the physics-step hook: tyre-frame longitudinal forces (lbf x 256).
    bool has_forces = false;
    int32_t fx[4] = {};
};

struct FfbComponents {
    double model_raw = 0;   // own aligning model before model_gain (computed even when the gain is 0)
    double game_hz = 30;    // measured GP4 update rate
    double texture = 0;     // kerb + gravel/grass rumble + front scrub + lock-up (added after the slew limit)
    double brake = 0;       // steering torque from unequal front braking forces (scrub radius)
    double damping_scale = 1;
    double game = 0, model = 0, road = 0, shift = 0, impact = 0, damping = 0, friction = 0, spring = 0;
    double total = 0, output = 0;
    double yaw_rate = 0, slip_front = 0, steer_rate = 0;
};

class FfbModel {
public:
    void configure(const FfbParams& p) { p_ = p; }
    const FfbParams& params() const { return p_; }
    void reset();
    // dt in seconds (>0). Returns the force to send, -1..1.
    double step(const FfbInput& in, double dt);
    const FfbComponents& last() const { return c_; }

private:
    FfbParams p_;
    FfbComponents c_;
    bool have_prev_ = false;
    double prev_steer_ = 0, steer_rate_f_ = 0;
    double game_f_ = 0;
    double game_dt_ = 1.0 / 30.0, last_change_t_ = -1;
    double kerb_phase_ = 0, scrub_phase_ = 0, noise_ = 0, lock_phase_ = 0, brake_f_ = 0;
    uint32_t rng_ = 0x12345678;
    int32_t last_yaw_seen_ = 0;
    int32_t prev_yaw_ = 0;
    double yaw_rate_f_ = 0;
    double prev_speed_ = 0, decel_f_ = 0;
    double road_prev_in_ = 0, road_hp_ = 0;
    int prev_gear_ = -1;
    double shift_t_ = 1e9, shift_sign_ = 1;
    double impact_t_ = 1e9, impact_amp_ = 0;
    double t_ = 0;
    double out_ = 0;
};

// Helpers (exposed for tests).
double lowpass_alpha(double cutoff_hz, double dt);
double wrap_turn(double x);   // wraps a fraction of a turn to [-0.5, 0.5)
double soft_clip(double x);   // linear to 0.8, then smooth saturation towards 1

}  // namespace gp4u
