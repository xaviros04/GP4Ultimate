// A UDP socket (IPv4) for the online server and the DirectPlay replacement; Windows
// (Winsock) and POSIX.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/net_server.h"   // Endpoint

namespace gp4u {
namespace net {

class Udp {
public:
    Udp() = default;
    ~Udp() { close(); }
    Udp(const Udp&) = delete;
    Udp& operator=(const Udp&) = delete;

    // Binds to every interface; port 0 picks a free one.
    bool open(uint16_t port, std::string* err);
    void close();
    bool is_open() const { return fd_ != kInvalid; }
    uint16_t local_port() const;

    bool send(const Endpoint& to, const uint8_t* d, size_t n);
    // Waits up to `timeout_ms` for a datagram: its size, 0 if none came, -1 on error.
    int recv(Endpoint& from, uint8_t* buf, size_t cap, int timeout_ms);

private:
#ifdef _WIN32
    using Fd = uintptr_t;
    static constexpr Fd kInvalid = ~(uintptr_t)0;
#else
    using Fd = int;
    static constexpr Fd kInvalid = -1;
#endif
    Fd fd_ = kInvalid;
};

// "host", "host:port", "1.2.3.4:5" (port defaults to `default_port`).
bool resolve(const std::string& text, uint16_t default_port, Endpoint& out, std::string* err);

// Seconds from a monotonic clock.
double seconds();

}  // namespace net
}  // namespace gp4u
