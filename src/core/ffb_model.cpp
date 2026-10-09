#include "ffb_model.h"

#include <algorithm>
#include <cmath>

namespace gp4u {

namespace gp4tex {
constexpr int kKerb = 1, kGrass = 3, kGravel = 4;   // GP4 surface codes (core/gp4_addresses.h)
}

static constexpr double kPi = 3.14159265358979323846;

namespace {
struct Loader {
    const Ini& ini;
    void operator()(const char* k, double& v) const { v = ini.get_double("ffb", k, v); }
    void operator()(const char* k, bool& v) const { v = ini.get_bool("ffb", k, v); }
    void operator()(const char* k, int& v) const { v = (int)ini.get_int("ffb", k, v); }
};
}  // namespace

void FfbParams::load(const Ini& ini) {
    for_each(Loader{ini});
    front_left = std::clamp(front_left, 0, 3);
    front_right = std::clamp(front_right, 0, 3);
    max_output = std::clamp(max_output, 0.0, 1.0);
    min_force = std::clamp(min_force, 0.0, 0.5);
    if (wheel_range_deg < 90) wheel_range_deg = 90;
}

double lowpass_alpha(double cutoff_hz, double dt) {
    if (cutoff_hz <= 0) return 1.0;
    double rc = 1.0 / (2.0 * kPi * cutoff_hz);
    return dt / (rc + dt);
}

double wrap_turn(double x) {
    x -= std::floor(x + 0.5);
    return x;
}

double soft_clip(double x) {
    double a = std::fabs(x);
    if (a <= 0.8) return x;
    double y = 0.8 + 0.2 * std::tanh((a - 0.8) / 0.2);
    return x < 0 ? -y : y;
}

void FfbModel::reset() {
    FfbParams p = p_;
    *this = FfbModel();
    p_ = p;
}

double FfbModel::step(const FfbInput& in, double dt) {
    if (!(dt > 0)) dt = 1e-3;
    if (dt > 0.1) dt = 0.1;
    t_ += dt;
    c_ = FfbComponents();

    // Wheel angular speed (rad/s of the physical wheel), filtered at 40 Hz.
    double half_range = p_.wheel_range_deg * kPi / 360.0;
    double rate = have_prev_ ? (in.steer - prev_steer_) * half_range / dt : 0.0;
    steer_rate_f_ += lowpass_alpha(40.0, dt) * (rate - steer_rate_f_);
    prev_steer_ = in.steer;
    c_.steer_rate = steer_rate_f_;

    double speed = in.car_valid ? std::max(0.0, in.speed_mps) : 0.0;
    // GP4's update rate: its car state (yaw) changes once per simulation step.
    if (in.car_valid && speed > 1.0 && in.yaw_raw != last_yaw_seen_) {
        if (last_change_t_ >= 0) {
            double iv = t_ - last_change_t_;
            if (iv > 0.004 && iv < 0.2) game_dt_ += 0.05 * (iv - game_dt_);
        }
        last_change_t_ = t_;
        last_yaw_seen_ = in.yaw_raw;
    }
    c_.game_hz = 1.0 / game_dt_;
    c_.damping_scale = p_.damping_adapt ? std::clamp(game_dt_ * 30.0, 0.55, 1.3) : 1.0;
    double low = std::max(0.0, 1.0 - speed / 15.0);
    double high = std::min(1.0, speed / 50.0);
    c_.damping = -(p_.damping + p_.damping_speed * c_.damping_scale * high + p_.damping_low_speed * low) * steer_rate_f_;
    c_.friction = -p_.friction * std::tanh(steer_rate_f_ / 0.3);

    if (!in.car_valid) {
        // Menus, replays, no car found yet: gentle centring so the wheel is not dead.
        c_.spring = -p_.no_car_centering * in.steer;
        have_prev_ = false;
        prev_gear_ = -1;
        game_f_ = 0;
        road_hp_ = 0;
    } else {
        // 1) GP4's own force value, upsampled with a low-pass. With the defaults g is exactly
        //    the DirectInput magnitude GP4 would send; converted to the physical convention.
        double mix = in.has_align ? std::clamp(p_.model_mix, 0.0, 1.0) : 0.0;
        double g = (in.game_ffb - mix * in.game_align) * p_.game_scale * (p_.game_invert ? -1.0 : 1.0);
        game_f_ += lowpass_alpha(p_.game_lowpass_hz, dt) * (g - game_f_);
        c_.game = kDirectInputSign * p_.game_gain * game_f_;
        // Front wheels on grass/gravel: lighter steering (GP4's own grip there is 0.5-0.75).
        if (in.has_wheels) {
            int off = 0;
            for (int w = 2; w < 4; ++w) off += in.surface[w] == gp4tex::kGrass || in.surface[w] == gp4tex::kGravel;
            c_.game *= 1.0 - std::clamp(p_.offroad_light, 0.0, 1.0) * off / 2.0;
        }

        // 2) Own aligning-torque estimate.
        if (have_prev_) {
            double dyaw = wrap_turn(((double)in.yaw_raw - (double)prev_yaw_) / p_.yaw_units_per_turn) * 2 * kPi;
            yaw_rate_f_ += lowpass_alpha(p_.yaw_rate_lowpass_hz, dt) * (dyaw / dt - yaw_rate_f_);
        }
        prev_yaw_ = in.yaw_raw;
        c_.yaw_rate = yaw_rate_f_;
        // Computed always ("shadow mode") so a CSV can calibrate it before it is switched on.
        if (speed > 2.0) {
            double beta = wrap_turn((double)in.vel_heading_raw / p_.heading_units_per_turn -
                                    (double)in.yaw_raw / p_.yaw_units_per_turn) * 2 * kPi;
            double delta = in.steer * p_.steer_lock_deg * kPi / 180.0;
            double alpha = std::atan2(speed * std::sin(beta) + p_.cg_to_front_m * yaw_rate_f_,
                                      speed * std::cos(beta)) - delta;
            double ap = std::max(0.5, p_.alpha_peak_deg) * kPi / 180.0;
            const double C = 1.9;
            double B = std::tan(kPi / (2 * C)) / ap;
            double fy = std::sin(C * std::atan(B * alpha));                 // -1..1, peaks at ap
            double trail = std::max(0.0, 1.0 - std::fabs(alpha) / (1.6 * ap)); // trail collapses past peak
            double load = 1.0 + p_.aero_load * speed * speed;
            double fade = std::min(1.0, speed / 10.0);
            c_.slip_front = alpha;
            c_.model_raw = fy * trail * load * fade;
            // The model stands in for the part of GP4's aligning term taken out above; same
            // scale and sign conventions as that term (1.30 = calibrated GP4 / model ratio).
            double as_game = p_.model_scale * (p_.game_scale / (1.5 / 10000.0)) * (p_.game_invert ? 1.0 : -1.0);
            c_.model = (p_.model_gain + p_.game_gain * mix * as_game) * c_.model_raw;
        }

        // 3) Road texture: high-passed left/right front suspension difference.
        double d = (double)(in.susp[p_.front_left] - in.susp[p_.front_right]) * p_.road_scale;
        if (have_prev_) {
            double a = 1.0 / (1.0 + 2 * kPi * p_.road_hp_hz * dt);
            road_hp_ = a * (road_hp_ + d - road_prev_in_);
        }
        road_prev_in_ = d;
        c_.road = p_.road_gain * road_hp_;

        // 4) Gear-change kick.
        if (prev_gear_ >= 0 && in.gear != prev_gear_ && in.gear != 0 && prev_gear_ != 0) {
            shift_t_ = 0;
            shift_sign_ = -shift_sign_;
        }
        prev_gear_ = in.gear;

        // 5) Impacts: sudden deceleration.
        if (have_prev_) {
            double decel = (prev_speed_ - speed) / dt;
            decel_f_ += lowpass_alpha(20.0, dt) * (decel - decel_f_);
            if (decel_f_ > p_.impact_g * 9.81 && impact_t_ > 0.15) {
                impact_t_ = 0;
                impact_amp_ = std::min(1.0, decel_f_ / (p_.impact_g * 9.81 * 3));
            }
        }
        prev_speed_ = speed;
        have_prev_ = true;
    }

    if (shift_t_ < 1.0) {
        double tau = std::max(1.0, p_.shift_ms) / 1000.0;
        c_.shift = p_.shift_kick * shift_sign_ * std::exp(-shift_t_ / tau);
        shift_t_ += dt;
    }
    if (impact_t_ < 0.3) {
        c_.impact = p_.impact_gain * impact_amp_ * std::sin(2 * kPi * 25.0 * impact_t_) * std::exp(-impact_t_ / 0.08);
        impact_t_ += dt;
    }

    // Unequal front braking forces through the scrub radius. Physical convention: braking
    // harder on the right pulls the wheel right. Fx updates at the frame rate: smoothed like
    // GP4's own signal.
    if (in.car_valid && in.has_forces) {
        double d = ((double)in.fx[2] - (double)in.fx[3]) / std::max(1.0, p_.brake_full);
        brake_f_ += lowpass_alpha(p_.game_lowpass_hz, dt) * (std::clamp(d, -1.0, 1.0) - brake_f_);
        c_.brake = p_.brake_gain * brake_f_;
    } else {
        brake_f_ = 0;
    }
    c_.total = c_.game + c_.model + c_.road + c_.shift + c_.impact + c_.damping + c_.friction + c_.spring + c_.brake;
    double x = kDirectInputSign * soft_clip(p_.master_gain * c_.total);
    if (p_.invert) x = -x;
    if (p_.min_force > 0 && std::fabs(x) > 1e-3) x = (x < 0 ? -1 : 1) * (p_.min_force + (1 - p_.min_force) * std::fabs(x));
    x = std::clamp(x, -p_.max_output, p_.max_output);
    double max_step = p_.slew_per_s * dt;
    out_ += std::clamp(x - out_, -max_step, max_step);
    // Textures (vibrations) after the slew limit, which would otherwise flatten them.
    if (in.car_valid && in.has_wheels && speed > 2.0) {
        double fade = std::min(1.0, speed / 20.0);
        int kerb_front = 0, kerb_rear = 0, gravel = 0, grass = 0;
        for (int w = 0; w < 4; ++w) {
            bool front = w >= 2;
            if (in.surface[w] == gp4tex::kKerb) (front ? kerb_front : kerb_rear)++;
            if (in.surface[w] == gp4tex::kGravel) gravel += front ? 2 : 1;
            if (in.surface[w] == gp4tex::kGrass) grass += front ? 2 : 1;
        }
        // Kerb: like a rumble strip, the frequency follows the speed (stripes ~1.5 m).
        double hz = std::clamp(speed / 1.5, 10.0, 40.0);
        kerb_phase_ = std::fmod(kerb_phase_ + 2 * kPi * hz * dt, 2 * kPi);
        if (kerb_front || kerb_rear) {
            double w = std::min(1.0, 0.6 + 0.2 * kerb_front + 0.1 * kerb_rear);
            c_.texture += p_.kerb_gain * w * fade * std::sin(kerb_phase_);
        }
        // Gravel / grass: band-limited noise.
        if (gravel || grass) {
            rng_ = rng_ * 1664525u + 1013904223u;
            double white = (double)(rng_ >> 8) / (double)(1u << 24) * 2.0 - 1.0;
            noise_ += lowpass_alpha(25.0, dt) * (white - noise_);
            double amt = p_.gravel_gain * std::min(1.0, gravel / 4.0) + p_.grass_gain * std::min(1.0, grass / 4.0);
            c_.texture += 2.5 * amt * fade * noise_;   // the low-passed noise is ~0.4 rms
        }
        // Front tyres sliding (understeer): fine scrub, proportional to how much they slide.
        double slide = (std::fabs((double)in.slide_lat[2]) + std::fabs((double)in.slide_lat[3])) * 0.5;
        double s = std::clamp(slide / std::max(1.0, p_.scrub_full), 0.0, 1.0);
        scrub_phase_ = std::fmod(scrub_phase_ + 2 * kPi * 55.0 * dt, 2 * kPi);
        if (s > 0) c_.texture += p_.scrub_gain * s * fade * std::sin(scrub_phase_);
        // A front wheel locking under braking: judder.
        double lock = std::max(std::fabs((double)in.slide_long[2]), std::fabs((double)in.slide_long[3]));
        double l = std::clamp(lock / std::max(1.0, p_.lockup_full), 0.0, 1.0);
        lock_phase_ = std::fmod(lock_phase_ + 2 * kPi * 22.0 * dt, 2 * kPi);
        if (l > 0) c_.texture += p_.lockup_gain * l * fade * (std::sin(lock_phase_) > 0 ? 1.0 : -1.0);
    }
    double final = out_ + kDirectInputSign * p_.master_gain * c_.texture;
    final = std::clamp(final, -p_.max_output, p_.max_output);
    c_.output = final;
    return final;
}

}  // namespace gp4u
