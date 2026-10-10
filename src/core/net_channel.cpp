#include "core/net_channel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gp4u {
namespace net {

namespace {

void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

constexpr uint32_t kWindow = 1024;   // reliable datagrams accepted ahead of the next expected

// Clock stamps: ms, never 0 (0 means "nothing to echo").
uint32_t stamp(double now) {
    uint32_t v = (uint32_t)(int64_t)(now * 1000.0);
    return v ? v : 1;
}

}  // namespace

bool peek_header(const uint8_t* d, size_t n, uint32_t& token, uint8_t& kind) {
    if (n < kHeaderSize || d[0] != 'G' || d[1] != '4' || d[2] != kWireVersion) return false;
    kind = d[3];
    if (kind < kUnreliable || kind > kUnreliablePart) return false;
    token = get32(d + 4);
    return true;
}

Bytes Channel::datagram(uint8_t kind, uint32_t seq, const uint8_t* p, size_t n, double now) {
    Bytes b(kHeaderSize + n);
    b[0] = 'G';
    b[1] = '4';
    b[2] = kWireVersion;
    b[3] = kind;
    put32(&b[4], token_);
    put32(&b[8], seq);
    uint32_t bits = 0;
    for (uint32_t i = 0; i < 32; ++i)
        if (early_.count(expect_ + 1 + i)) bits |= 1u << i;
    put32(&b[12], expect_);
    put32(&b[16], bits);
    put32(&b[20], stamp(now));
    if (have_peer_stamp_) {
        double held = std::min(65.0, std::max(0.0, now - peer_stamp_at_)) * 1000;
        put32(&b[24], peer_stamp_);
        b[28] = (uint8_t)(int)held;
        b[29] = (uint8_t)((int)held >> 8);
    }
    if (n) std::memcpy(&b[kHeaderSize], p, n);
    ack_due_ = false;   // this datagram carries the acknowledgement
    return b;
}

void Channel::transmit(uint32_t seq, Pending& p, double now, std::vector<Bytes>& out) {
    out.push_back(datagram(kReliable, seq, p.payload.data(), p.payload.size(), now));
    if (p.tries++ == 0) p.first = now;
    else ++stats_.resent;
    p.last = now;
    last_sent_ = now;
    ++stats_.sent;
}

void Channel::send(const uint8_t* d, size_t n, bool reliable, double now, std::vector<Bytes>& out) {
    if (n > kMaxMessage) return;
    uint16_t count = (uint16_t)std::max<size_t>(1, (n + kFragmentData - 1) / kFragmentData);
    if (!reliable && count == 1) {
        out.push_back(datagram(kUnreliable, 0, d, n, now));
        last_sent_ = now;
        ++stats_.sent;
        return;
    }
    if (!reliable) {
        uint32_t id = next_unreliable_++;
        for (uint16_t i = 0; i < count; ++i) {
            size_t off = (size_t)i * kFragmentData, len = std::min(kFragmentData, n - off);
            Bytes p(4 + len);
            p[0] = (uint8_t)i;
            p[1] = (uint8_t)(i >> 8);
            p[2] = (uint8_t)count;
            p[3] = (uint8_t)(count >> 8);
            std::memcpy(&p[4], d + off, len);
            out.push_back(datagram(kUnreliablePart, id, p.data(), p.size(), now));
            ++stats_.sent;
        }
        last_sent_ = now;
        return;
    }
    for (uint16_t i = 0; i < count; ++i) {
        size_t off = (size_t)i * kFragmentData, len = std::min(kFragmentData, n - std::min(n, off));
        Pending p;
        p.payload.resize(4 + len);
        p.payload[0] = (uint8_t)i;
        p.payload[1] = (uint8_t)(i >> 8);
        p.payload[2] = (uint8_t)count;
        p.payload[3] = (uint8_t)(count >> 8);
        if (len) std::memcpy(&p.payload[4], d + off, len);
        uint32_t seq = next_seq_++;
        transmit(seq, pending_[seq] = std::move(p), now, out);
    }
}

void Channel::rtt_sample(double s) {
    if (srtt_ <= 0) {
        srtt_ = s;
        rttvar_ = s / 2;
    } else {
        rttvar_ = 0.75 * rttvar_ + 0.25 * std::fabs(srtt_ - s);
        srtt_ = 0.875 * srtt_ + 0.125 * s;
    }
}

double Channel::rto() const {
    if (srtt_ <= 0) return 0.25;
    // The measured round trip leaves out how long the peer held its ack (kAckDelay).
    return std::min(1.0, std::max(0.05, srtt_ + 4 * rttvar_ + kAckDelay + 0.005));
}

void Channel::on_ack(uint32_t ack, uint32_t bits, double now) {
    auto done = [&](std::map<uint32_t, Pending>::iterator it) {
        ++stats_.acks;
        return pending_.erase(it);
    };
    for (auto it = pending_.begin(); it != pending_.end() && it->first < ack;) it = done(it);
    for (uint32_t i = 0; bits && i < 32; ++i)
        if (bits & (1u << i)) {
            auto it = pending_.find(ack + 1 + i);
            if (it != pending_.end()) done(it);
        }
}

bool Channel::receive(const uint8_t* d, size_t n, double now, std::vector<Bytes>& in, std::vector<Bytes>& out) {
    uint32_t token;
    uint8_t kind;
    if (!peek_header(d, n, token, kind) || token != token_) return false;
    last_heard_ = now;
    on_ack(get32(d + 12), get32(d + 16), now);
    peer_stamp_ = get32(d + 20);
    peer_stamp_at_ = now;
    have_peer_stamp_ = peer_stamp_ != 0;
    uint32_t echo = get32(d + 24);
    if (echo && echo != sampled_echo_) {   // each of our stamps times one round trip
        sampled_echo_ = echo;
        double rtt = (int32_t)(stamp(now) - echo) / 1000.0 - (d[28] | d[29] << 8) / 1000.0;
        if (rtt >= 0 && rtt < 30) rtt_sample(rtt);
    }
    const uint8_t* p = d + kHeaderSize;
    size_t len = n - kHeaderSize;
    if (kind == kUnreliable) {
        in.emplace_back(p, p + len);
        ++stats_.received;
        return true;
    }
    if (kind == kAck) return true;
    if (kind == kUnreliablePart) {
        if (len < 4) return false;
        uint32_t id = get32(d + 8);
        uint16_t frag = (uint16_t)(p[0] | p[1] << 8), count = (uint16_t)(p[2] | p[3] << 8);
        if (count < 2 || frag >= count || (size_t)count * kFragmentData > kMaxMessage + kFragmentData) return false;
        Parts& m = parts_[id];
        if (m.count && m.count != count) return false;
        if (!m.count) m.count = count, m.part.resize(count), m.have.resize(count);
        if (!m.have[frag]) {   // (a duplicate is ignored)
            m.part[frag].assign(p + 4, p + len);
            m.have[frag] = true;
            ++m.got;
        }
        if (m.got == m.count) {
            Bytes msg;
            for (auto& b : m.part) msg.insert(msg.end(), b.begin(), b.end());
            in.push_back(std::move(msg));
            ++stats_.received;
            parts_.erase(id);
        }
        // Keep the last few messages only: an older one missing a part never completes.
        while (parts_.size() > 8) {
            parts_.erase(parts_.begin());
            ++stats_.parts_dropped;
        }
        return true;
    }
    // Reliable.
    if (len < 4) return false;
    uint32_t seq = get32(d + 8);
    if (!ack_due_) ack_at_ = now + kAckDelay;
    ack_due_ = true;
    if (seq < expect_ || early_.count(seq)) {
        ++stats_.duplicates;
        return true;
    }
    if (seq - expect_ >= kWindow) return true;   // too far ahead: the peer will resend
    early_[seq] = Bytes(p, p + len);
    ++stats_.received;
    // Deliver what is now in order.
    for (auto it = early_.find(expect_); it != early_.end(); it = early_.find(expect_)) {
        const Bytes& f = it->second;
        uint16_t frag = (uint16_t)(f[0] | f[1] << 8), count = (uint16_t)(f[2] | f[3] << 8);
        bool fits = count > 0 && frag < count && partial_.size() + f.size() - 4 <= kMaxMessage;
        if (frag == 0) {
            partial_.clear();
            partial_count_ = count;
            partial_next_ = 0;
        }
        if (fits && frag == partial_next_ && count == partial_count_) {
            partial_.insert(partial_.end(), f.begin() + 4, f.end());
            if (++partial_next_ == count) {
                in.push_back(std::move(partial_));
                partial_.clear();
                partial_next_ = partial_count_ = 0;
            }
        } else {
            partial_.clear();   // broken sequence: drop the message
            partial_next_ = partial_count_ = 0;
        }
        early_.erase(it);
        ++expect_;
    }
    (void)out;
    return true;
}

void Channel::poll(double now, std::vector<Bytes>& out) {
    double base = rto();
    for (auto& p : pending_) {
        double wait = std::min(kMaxResendWait, base * std::pow(2.0, std::min(p.second.tries - 1, 4)));
        if (now - p.second.last >= wait) transmit(p.first, p.second, now, out);
    }
    if ((ack_due_ && now >= ack_at_) || now - last_sent_ >= kKeepAlive) {
        out.push_back(datagram(kAck, 0, nullptr, 0, now));
        last_sent_ = now;
    }
}

void Channel::flush_ack(std::vector<Bytes>& out) {
    if (ack_due_) out.push_back(datagram(kAck, 0, nullptr, 0, last_heard_));
}

bool Channel::failed(double now) const {
    return !pending_.empty() && pending_.begin()->second.tries > 0 && now - pending_.begin()->second.first > kGiveUp;
}

}  // namespace net
}  // namespace gp4u
