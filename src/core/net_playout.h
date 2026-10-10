// Playout buffer for GP4's race packets (used by win/net_hook.cpp in free-running races).
//
// Each machine sends one state packet per simulation step, but they arrive with jitter:
// one step brings none and the next brings two, so a remote car stops for a frame and
// then jumps two (the "flicker" seen in the first free-running race). The DirectPlay
// Receive hook pulls everything that has arrived into this buffer and hands GP4 at most
// one race state packet per sender per simulation step, keeping one in reserve for the
// step that brings none. A sender that gets ahead (2+ still waiting after the step's
// packet) is caught up by delivering the extra ones at once, so the added delay stays
// within a step.
// Everything else (menus, start handshake, DirectPlay system messages) goes straight
// through, and so does everything when no race is being stepped.
//
// Pure C++, no Windows: unit-tested in tests/test_core.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace gp4u {

class NetPlayout {
public:
    struct Msg {
        uint32_t from = 0, to = 0;
        std::vector<uint8_t> data;
        uintptr_t origin = 0;   // where it was received: DirectPlay system messages point into it
    };

    // GP4 race state packet (type 1, not a menu message, not from DirectPlay itself).
    static bool is_race_state(uint32_t from, const uint8_t* d, size_t n) {
        return from != 0 && n >= 8 && (d[3] & 0x80) == 0 && d[5] == 1;
    }

    // A packet that only carries state (cars, clock, seed): an older one is worthless once a
    // newer one is there. Not those that announce something (a player leaving, the race's
    // events, a departure mask), which GP4 must see.
    static bool state_only(const uint8_t* d, size_t n) {
        if (n < 12 || (d[6] & 0x4C)) return false;
        uint32_t flags = (uint32_t)d[8] | (uint32_t)d[9] << 8 | (uint32_t)d[10] << 16 | (uint32_t)d[11] << 24;
        if (flags & 0x20) {
            if (n < 16) return false;
            if (d[12] | d[13] | d[14] | d[15]) return false;
        }
        return true;
    }

    void push(Msg m) {
        if (!is_race_state(m.from, m.data.data(), m.data.size())) {
            now_.push_back(std::move(m));
            return;
        }
        auto& q = paced_[m.from].q;
        q.push_back(std::move(m));
        // After a pause of this PC (a stall, a loading screen) hundreds of packets wait;
        // handing them all to GP4 in one step is what preceded two crashes. Keep the newest.
        size_t plain = 0;
        for (auto& x : q) plain += state_only(x.data.data(), x.data.size());
        for (auto it = q.begin(); plain > kMaxHeld && it != q.end();) {
            if (state_only(it->data.data(), it->data.size())) {
                it = q.erase(it);
                --plain;
                ++dropped_;
            } else {
                ++it;
            }
        }
    }

    // A simulation step starts: each sender may deliver one more packet.
    void begin_step(double now_ms) {
        last_step_ms_ = now_ms;
        stepping_ = true;
        ++step_;
    }

    // Next message for GP4, if any is due now.
    bool pop(double now_ms, Msg& out) {
        if (!now_.empty()) {
            out = std::move(now_.front());
            now_.pop_front();
            return true;
        }
        bool racing = stepping_ && now_ms - last_step_ms_ < kIdleMs;
        for (auto& p : paced_) {
            Sender& s = p.second;
            if (s.q.empty()) continue;
            // Not racing: everything goes. Racing: the step's packet, or a backlog.
            bool due = s.used != step_;
            if (!racing || due || s.q.size() >= 2) {
                if (racing && due) s.prev_used = s.used, s.used = step_;
                else if (racing) ++caught_up_;
                out = std::move(s.q.front());
                s.q.pop_front();
                ++delivered_;
                return true;
            }
        }
        return false;
    }

    // Put back a message GP4 could not take (its buffer was too small).
    void unpop(Msg m) {
        if (is_race_state(m.from, m.data.data(), m.data.size())) {
            auto& s = paced_[m.from];
            s.q.push_front(std::move(m));
            if (s.used == step_) s.used = s.prev_used;
            --delivered_;
        } else {
            now_.push_front(std::move(m));
        }
    }

    size_t held(uint32_t from) const {
        auto it = paced_.find(from);
        return it == paced_.end() ? 0 : it->second.q.size();
    }
    unsigned delivered() const { return delivered_; }
    unsigned caught_up() const { return caught_up_; }
    unsigned dropped() const { return dropped_; }   // stale packets left out of a backlog

    static constexpr double kIdleMs = 250;   // no step for this long: not racing
    static constexpr size_t kMaxHeld = 4;    // state-only race packets kept per sender

private:
    struct Sender {
        std::deque<Msg> q;
        uint64_t used = ~0ull, prev_used = ~0ull;   // step whose packet was delivered
    };
    std::deque<Msg> now_;
    std::map<uint32_t, Sender> paced_;
    bool stepping_ = false;
    double last_step_ms_ = 0;
    uint64_t step_ = 0;
    unsigned delivered_ = 0, caught_up_ = 0, dropped_ = 0;
};

}  // namespace gp4u
