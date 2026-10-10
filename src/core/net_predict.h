// Prediction of the other players' cars when their packet is late (win/net_sync.cpp).
//
// GP4 does not simulate the other players' cars: every step, the record that arrived in
// their packet (the first 0x2AC bytes of the car, 0x44580c) replaces the car. When a
// packet is late, the car stands still for that step and then jumps. GP4 simulates every
// AI car on every PC, though, with its per-car update (0x435b61), and the cars of the
// other players skip it only because they are marked as network cars (+0x414 & 0x80).
// With prediction on, a remote car whose record did not change in a step (no packet
// applied) gets that same update, with the inputs of its last packet still in the record:
// GP4's own physics carries it on until the next packet corrects it. At most `max_steps`
// steps in a row (a player who stopped sending is left standing).
//
// This class only decides; the caller runs GP4's update. Pure C++: tests/test_core.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gp4u {

class RemotePredictor {
public:
    static constexpr uint32_t kNetCar = 0x80;     // +0x414: car driven over the network
    static constexpr uint32_t kLocalCar = 0x40;   // +0x414: driven on this PC
    static constexpr uint32_t kGone = 0x04;       // +0xe5: its player left

    RemotePredictor(size_t cars = 22, size_t bytes = 0x2AC, int max_steps = 30)
        : bytes_(bytes), max_(max_steps), last_(cars), run_(cars, 0), live_(cars, false) {}

    // Before the step's update: should car i be advanced by GP4's update this step?
    // rec: the car record; flags414 / flagse5: its bytes at +0x414 and +0xe5.
    bool due(size_t i, const uint8_t* rec, uint8_t flags414, uint8_t flagse5) {
        if (i >= last_.size()) return false;
        bool remote = (flags414 & kNetCar) && !(flags414 & kLocalCar) && !(flagse5 & kGone);
        if (!remote) {
            forget(i);
            return false;
        }
        bool changed = last_[i].size() != bytes_ || std::memcmp(last_[i].data(), rec, bytes_) != 0;
        if (changed) {
            // A packet replaced it (or this is the first look): no prediction needed. The
            // first change after the first look means packets flow for this car.
            if (!last_[i].empty()) live_[i] = true;
            run_[i] = 0;
            return false;
        }
        if (!live_[i] || run_[i] >= max_) return false;
        ++run_[i];
        ++predicted_;
        return true;
    }

    // After the step (prediction included): remember the record to spot the next packet.
    void seen(size_t i, const uint8_t* rec) {
        if (i < last_.size()) last_[i].assign(rec, rec + bytes_);
    }

    void forget(size_t i) {
        last_[i].clear();
        run_[i] = 0;
        live_[i] = false;
    }
    void reset() {
        for (size_t i = 0; i < last_.size(); ++i) forget(i);
    }

    unsigned predicted() const { return predicted_; }   // steps predicted, all cars, in total
    int run(size_t i) const { return run_[i]; }

private:
    size_t bytes_;
    int max_;
    std::vector<std::vector<uint8_t>> last_;
    std::vector<int> run_;
    std::vector<bool> live_;
    unsigned predicted_ = 0;
};

}  // namespace gp4u
