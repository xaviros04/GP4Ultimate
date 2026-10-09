// Finds GP4's array of CarDynamicData records in a block of memory, and confirms which
// record is the player's car by correlating its steering field with our own input.
// Pure C++: the Windows side feeds it memory copies (ReadProcessMemory on itself).
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

namespace gp4u {

struct CarArrayCandidate {
    uint32_t base = 0;        // address of the first plausible record
    uint32_t count = 0;       // consecutive plausible records at `stride`
    std::vector<uint32_t> players;  // addresses of records with id >= 0x80
};

// Scans `bytes` (a copy of memory starting at address `base_addr`) for runs of at least
// `min_run` plausible records spaced `stride` bytes apart. Starts are 4-byte aligned.
// Overlapping runs are reported once (the earliest start).
std::vector<CarArrayCandidate> find_car_arrays(const uint8_t* bytes, uint32_t size, uint32_t base_addr,
                                               uint32_t stride, uint32_t min_run);

// Pearson correlation of two equally long series; 0 when either is constant.
double correlation(const std::vector<double>& a, const std::vector<double>& b);

// Rolling confirmation: feed (our steering input, steering read from the candidate)
// pairs; confirmed once |r| >= threshold over a window with enough steering movement.
class SteerCorrelator {
public:
    explicit SteerCorrelator(size_t window = 400, double threshold = 0.85, double min_span = 0.15)
        : window_(window), threshold_(threshold), min_span_(min_span) {}
    void reset() { in_.clear(); game_.clear(); }
    void add(double input_norm, double game_value);
    bool ready() const;        // window full, the input moved AND the game value moved
    double r() const;          // current correlation
    bool confirmed() const { return ready() && (r() >= threshold_ || r() <= -threshold_); }
    // Only a candidate whose value moves *differently* from the input is rejected; one that
    // does not move at all (menus, pause, car under automatic control) stays undecided.
    bool rejected() const { return ready() && !confirmed(); }

private:
    size_t window_;
    double threshold_, min_span_;
    std::deque<double> in_, game_;
};

}  // namespace gp4u
