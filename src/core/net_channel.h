// One UDP conversation between a player and the GP4 Ultimate server, with optional
// guaranteed, in-order delivery (what DirectPlay calls DPSEND_GUARANTEED).
//
// Every datagram starts with a 32-byte header:
//   'G' '4' version kind | token u32 | seq u32 | ack u32 | ack_bits u32 |
//   stamp u32 | echo u32 | held u16 | reserved u16
// - token: chosen by the player, identifies the conversation (survives a NAT port change);
// - kind 1 unreliable: the payload is one message, delivered as it comes (GP4's race state);
// - kind 2 reliable: seq numbers the datagram; the payload is u16 fragment, u16 count, data.
//   Messages longer than one datagram are split. Reliable messages come out in order, once;
// - kind 3 ack only (also the keep-alive, once a second when nothing else is sent);
// - kind 4 part of an unreliable message too long for one datagram (the race master's
//   1400-byte packets): seq numbers the message, the payload is as in kind 2. It comes out
//   once all its parts have arrived; if one is lost the message is lost, as with DirectPlay.
//   Never resent: a late race packet is worse than a missing one.
// ack/ack_bits ride on every datagram: everything below `ack` arrived, and bit i means
// ack+1+i arrived too. stamp is the sender's clock (ms); echo returns the stamp of the
// last datagram received and held how long ago it arrived, which times the round trip.
// Unacknowledged datagrams are resent after a timeout derived from that round trip.
//
// Pure C++, no sockets: the owner moves datagrams in and out (tests/test_online.cpp drives
// it through a simulated lossy network).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace gp4u {
namespace net {

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kWireVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kFragmentData = 1200;           // payload bytes per datagram
constexpr size_t kMaxMessage = 64 * 1024;        // longest message accepted
constexpr size_t kMaxDatagram = kHeaderSize + 4 + kFragmentData;

enum Kind : uint8_t { kUnreliable = 1, kReliable = 2, kAck = 3, kUnreliablePart = 4 };

// Header fields of a datagram, without accepting it (to route it to its conversation).
bool peek_header(const uint8_t* d, size_t n, uint32_t& token, uint8_t& kind);

class Channel {
public:
    Channel() = default;
    explicit Channel(uint32_t token) : token_(token) {}

    uint32_t token() const { return token_; }

    // Queues a message; the datagrams to transmit now are appended to `out`.
    void send(const uint8_t* d, size_t n, bool reliable, double now, std::vector<Bytes>& out);
    void send(const Bytes& m, bool reliable, double now, std::vector<Bytes>& out) {
        send(m.data(), m.size(), reliable, now, out);
    }

    // A datagram from the peer. Complete messages are appended to `in`, and anything
    // that must go back right away to `out`. False if it is not a valid datagram.
    bool receive(const uint8_t* d, size_t n, double now, std::vector<Bytes>& in, std::vector<Bytes>& out);

    // Resends, pending acks and keep-alives. Call every few milliseconds.
    void poll(double now, std::vector<Bytes>& out);
    // The acknowledgement owed for what was received, now (before the channel is dropped).
    void flush_ack(std::vector<Bytes>& out);

    double last_heard() const { return last_heard_; }
    double rtt() const { return srtt_; }             // seconds, 0 until measured
    double rtt_var() const { return rttvar_; }       // its mean deviation (jitter), seconds
    size_t unacked() const { return pending_.size(); }
    // A reliable message went unacknowledged for `kGiveUp` seconds: the peer is gone.
    bool failed(double now) const;

    struct Stats {
        unsigned sent = 0, resent = 0, received = 0, duplicates = 0, acks = 0, parts_dropped = 0;
    };
    const Stats& stats() const { return stats_; }

    static constexpr double kAckDelay = 0.010;
    static constexpr double kKeepAlive = 1.0;
    static constexpr double kGiveUp = 20.0;
    static constexpr double kMaxResendWait = 1.0;

private:
    struct Pending {
        Bytes payload;   // fragment header + data
        double first = 0, last = 0;
        int tries = 0;
    };
    Bytes datagram(uint8_t kind, uint32_t seq, const uint8_t* p, size_t n, double now);
    void transmit(uint32_t seq, Pending& p, double now, std::vector<Bytes>& out);
    void on_ack(uint32_t ack, uint32_t bits, double now);
    void rtt_sample(double s);
    double rto() const;

    uint32_t token_ = 0;
    // Sending.
    uint32_t next_seq_ = 1;
    std::map<uint32_t, Pending> pending_;
    double srtt_ = 0, rttvar_ = 0;
    double last_sent_ = -1e9;
    // Receiving.
    uint32_t expect_ = 1;
    uint32_t peer_stamp_ = 0, sampled_echo_ = 0;
    double peer_stamp_at_ = 0;
    bool have_peer_stamp_ = false;
    std::map<uint32_t, Bytes> early_;   // arrived ahead of `expect_`
    Bytes partial_;
    uint16_t partial_count_ = 0, partial_next_ = 0;
    // Unreliable messages in parts, by message number (only the last few are kept).
    struct Parts {
        uint16_t count = 0, got = 0;
        std::vector<Bytes> part;
        std::vector<bool> have;
    };
    std::map<uint32_t, Parts> parts_;
    uint32_t next_unreliable_ = 1;
    bool ack_due_ = false;
    double ack_at_ = 0;
    double last_heard_ = -1e9;
    Stats stats_;
};

}  // namespace net
}  // namespace gp4u
