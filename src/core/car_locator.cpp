#include "car_locator.h"

#include <algorithm>
#include <cmath>

#include "car_data.h"

namespace gp4u {

std::vector<CarArrayCandidate> find_car_arrays(const uint8_t* bytes, uint32_t size, uint32_t base_addr,
                                               uint32_t stride, uint32_t min_run) {
    std::vector<CarArrayCandidate> out;
    if (size < car::kSize || stride < car::kSize || min_run == 0) return out;
    // covered[o] marks record starts already reported, so a run is not re-reported
    // from its second, third, ... record.
    std::vector<bool> covered(size / 4 + 1, false);
    for (uint32_t off = 0; off + car::kSize <= size; off += 4) {
        if (covered[off / 4]) continue;
        if (!car::plausible(bytes + off)) continue;
        uint32_t n = 0;
        uint32_t o = off;
        while (o + car::kSize <= size && car::plausible(bytes + o)) {
            ++n;
            o += stride;
        }
        if (n < min_run) continue;
        CarArrayCandidate c;
        c.base = base_addr + off;
        c.count = n;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t ro = off + i * stride;
            covered[ro / 4] = true;
            if (bytes[ro + car::kId] & 0x80) c.players.push_back(base_addr + ro);
        }
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const CarArrayCandidate& a, const CarArrayCandidate& b) {
        return a.count > b.count;
    });
    return out;
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) {
        double da = a[i] - ma, db = b[i] - mb;
        sab += da * db; saa += da * da; sbb += db * db;
    }
    if (saa <= 1e-12 || sbb <= 1e-12) return 0;
    return sab / std::sqrt(saa * sbb);
}

void SteerCorrelator::add(double input_norm, double game_value) {
    in_.push_back(input_norm);
    game_.push_back(game_value);
    while (in_.size() > window_) { in_.pop_front(); game_.pop_front(); }
}

bool SteerCorrelator::ready() const {
    if (in_.size() < window_) return false;
    auto mm = std::minmax_element(in_.begin(), in_.end());
    if ((*mm.second - *mm.first) < min_span_) return false;
    auto gm = std::minmax_element(game_.begin(), game_.end());
    return *gm.second > *gm.first;
}

double SteerCorrelator::r() const {
    return correlation(std::vector<double>(in_.begin(), in_.end()), std::vector<double>(game_.begin(), game_.end()));
}

}  // namespace gp4u
