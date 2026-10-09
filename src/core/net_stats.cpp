#include "net_stats.h"

#include <cstdio>
#include <cstring>

namespace gp4u {

std::string NetStats::kind_of(const uint8_t* data, uint32_t size) {
    if (!data || size < 4) return "short";
    uint32_t h;
    std::memcpy(&h, data, 4);
    if (h & 0x80000000u) return "menu" + std::to_string((h >> 24) & 0x7F);
    return "race";
}

std::string NetStats::hex(const uint8_t* data, uint32_t size, uint32_t max_bytes) {
    std::string s;
    char b[4];
    for (uint32_t i = 0; i < size && i < max_bytes; ++i) {
        std::snprintf(b, sizeof b, "%02x", data[i]);
        if (i) s += (i % 4 == 0) ? "  " : " ";
        s += b;
    }
    if (size > max_bytes) s += " ...";
    return s;
}

void NetStats::count_kind(const std::string& k, const char* dir, const uint8_t* data, uint32_t size, uint32_t peer) {
    std::string key = std::string(dir) + " " + k;
    ++cur_.kinds[key];
    int& n = dumped_[key];
    if (n < dumps_per_kind_ && data) {
        ++n;
        char head[96];
        std::snprintf(head, sizeof head, "packet %s %s 0x%08x, %u bytes: ", dir, k.c_str(), peer, size);
        pending_.push_back(head + hex(data, size));
    }
}

void NetStats::on_send(double t, uint32_t to, uint32_t flags, const uint8_t* data, uint32_t size) {
    if (start_ < 0) start_ = t;
    ++cur_.sent;
    if (flags & 1) ++cur_.sent_guaranteed;   // DPSEND_GUARANTEED
    cur_.sent_bytes += size;
    count_kind(kind_of(data, size), "out", data, size, to);
}

void NetStats::on_recv(double t, uint32_t from, const uint8_t* data, uint32_t size) {
    if (start_ < 0) start_ = t;
    Peer& p = cur_.from[from];
    ++p.packets;
    p.bytes += size;
    auto it = last_seen_.find(from);
    if (it != last_seen_.end()) {
        double g = t - it->second;
        p.gap_sum += g;
        ++p.gaps;
        if (g > p.gap_max) p.gap_max = g;
    }
    last_seen_[from] = t;
    count_kind(kind_of(data, size), "in", data, size, from);
}

std::vector<std::string> NetStats::poll(double t, double period) {
    std::vector<std::string> out;
    out.swap(pending_);
    if (start_ < 0 || t - start_ < period) return out;
    double dt = t - start_;
    bool traffic = cur_.sent || !cur_.from.empty();
    if (traffic) {
        char b[256];
        std::snprintf(b, sizeof b, "net %.1fs: out %.0f pkt/s %.1f KB/s (guaranteed %u)", dt, cur_.sent / dt,
                      cur_.sent_bytes / dt / 1024.0, cur_.sent_guaranteed);
        std::string line = b;
        for (auto& kv : cur_.from) {
            const Peer& p = kv.second;
            std::snprintf(b, sizeof b, " | in 0x%08x %.0f pkt/s %.1f KB/s gap avg %.0f max %.0f ms", kv.first,
                          p.packets / dt, p.bytes / dt / 1024.0, p.gaps ? p.gap_sum / p.gaps * 1000 : 0.0,
                          p.gap_max * 1000);
            line += b;
        }
        std::snprintf(b, sizeof b, " | empty polls %llu |", cur_.empty_polls);
        line += b;
        for (auto& kv : cur_.kinds) line += " " + kv.first + "=" + std::to_string(kv.second);
        out.push_back(line);
    }
    cur_ = Period();
    start_ = t;
    return out;
}

}  // namespace gp4u
