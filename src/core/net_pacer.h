// Clock pacing for the free-running race network (win/net_sync.cpp).
//
// GP4 keeps the machines of an online race in step by waiting, every simulation step, for
// the other machine's packet (docs/GP4-FINDINGS.md "Red"). That costs one round trip per
// step: at 40 ms of ping the game runs at ~24 steps/s instead of 60, in slow motion.
// The mod lets every machine run at its own frame rate instead and keeps the game clocks
// together loosely: a client compares its race time ([0x66d814], milliseconds) with the
// race master's, extrapolated from the master's last packet with the real time elapsed
// since it arrived. If it is ahead by more than `window_ms` it waits a little; if it is
// behind by more than that it runs extra simulation steps (GP4's own catch-up path,
// [0x62f8f9]). The master never waits.
//
// Pure C++, no Windows: unit-tested in tests/test_core.cpp.
#pragma once
#include <cstdint>

namespace gp4u {

struct NetPacerConfig {
    // Tolerated clock difference before acting (game ms). Centred on 0, a client runs about
    // half a ping behind the master, so the master's packets (its car, the AI corrections
    // and the random seed) arrive when the client's clock reaches their time; a narrow
    // window keeps the AI corrections within a step or two.
    double window_ms = 30;
    double max_wait_ms = 50;   // longest wait in one step when ahead
    int max_catchup = 3;       // extra steps per frame when behind
    double hold_ms = 500;      // master silent longer than this: no pacing (run freely)
};

class NetPacer {
public:
    enum class Action { Run, Wait, CatchUp };

    explicit NetPacer(const NetPacerConfig& c = {}) : cfg_(c) {}

    void reset() { *this = NetPacer(cfg_); }
    const NetPacerConfig& config() const { return cfg_; }

    // Race time from a master packet that arrived at real time `now_ms`.
    void on_master(int32_t t, double now_ms) {
        if (have_ && (t < t_ - 2000 || t > t_ + 60000)) anchor_ok_ = false;   // new session / jump
        if (!anchor_ok_) {
            anchor_t_ = t;
            anchor_ms_ = now_ms;
            anchor_ok_ = true;
        } else if (now_ms - anchor_ms_ >= 1000) {
            double r = (double)(t - anchor_t_) / (now_ms - anchor_ms_);
            if (r > 0.25 && r < 4) rate_ = rate_known_ ? 0.7 * rate_ + 0.3 * r : r, rate_known_ = true;
            anchor_t_ = t;
            anchor_ms_ = now_ms;
        }
        t_ = t;
        rx_ms_ = now_ms;
        have_ = true;
    }

    // Master race time now, estimated; false if unknown or the master went silent.
    bool master_now(double now_ms, double& out) const {
        if (!have_ || now_ms - rx_ms_ > cfg_.hold_ms) return false;
        double el = now_ms - rx_ms_;
        out = t_ + (el > 0 ? el : 0) * rate_;
        return true;
    }

    // How far my race time is ahead of the master's (negative = behind).
    bool lead(int32_t my_t, double now_ms, double& out) const {
        double m;
        if (!master_now(now_ms, m)) return false;
        out = my_t - m;
        return true;
    }

    // `chain` = extra steps already run in a row before this one.
    Action decide(int32_t my_t, double now_ms, int chain) const {
        double l;
        if (!lead(my_t, now_ms, l)) return Action::Run;
        if (l > cfg_.window_ms) return Action::Wait;
        if (l < -cfg_.window_ms && chain < cfg_.max_catchup) return Action::CatchUp;
        return Action::Run;
    }

    double rate() const { return rate_; }
    bool has_master() const { return have_; }

private:
    NetPacerConfig cfg_;
    bool have_ = false;
    int32_t t_ = 0;
    double rx_ms_ = 0;
    double rate_ = 1.0;   // master race ms per real ms (1 when the game runs in real time)
    bool rate_known_ = false;
    bool anchor_ok_ = false;
    int32_t anchor_t_ = 0;
    double anchor_ms_ = 0;
};

}  // namespace gp4u
