#include "net/udp.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace gp4u {
namespace net {

namespace {

#ifdef _WIN32
bool winsock() {
    static bool ok = [] {
        WSADATA w;
        return WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    return ok;
}
std::string last_error() { return "error " + std::to_string(WSAGetLastError()); }
#else
bool winsock() { return true; }
std::string last_error() { return std::strerror(errno); }
#endif

sockaddr_in to_sockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(e.ip);
    a.sin_port = htons(e.port);
    return a;
}

}  // namespace

bool Udp::open(uint16_t port, std::string* err) {
    close();
    if (!winsock()) {
        if (err) *err = "Winsock not available";
        return false;
    }
    auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        if (err) *err = "socket: " + last_error();
        return false;
    }
    fd_ = (Fd)s;
#ifdef _WIN32
    // Otherwise an ICMP "port unreachable" makes the next recvfrom fail.
    BOOL no = FALSE;
    DWORD ret = 0;
    WSAIoctl((SOCKET)fd_, SIO_UDP_CONNRESET, &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
#endif
    int big = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof big);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0) {
        if (err) *err = "port " + std::to_string(port) + ": " + last_error();
        close();
        return false;
    }
    return true;
}

void Udp::close() {
    if (fd_ == kInvalid) return;
#ifdef _WIN32
    closesocket((SOCKET)fd_);
#else
    ::close(fd_);
#endif
    fd_ = kInvalid;
}

uint16_t Udp::local_port() const {
    sockaddr_in a{};
    socklen_t n = sizeof a;
    if (fd_ == kInvalid || getsockname(fd_, (sockaddr*)&a, &n) != 0) return 0;
    return ntohs(a.sin_port);
}

bool Udp::send(const Endpoint& to, const uint8_t* d, size_t n) {
    if (fd_ == kInvalid) return false;
    sockaddr_in a = to_sockaddr(to);
    return sendto(fd_, (const char*)d, (int)n, 0, (sockaddr*)&a, sizeof a) == (int)n;
}

int Udp::recv(Endpoint& from, uint8_t* buf, size_t cap, int timeout_ms) {
    if (fd_ == kInvalid) return -1;
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET)fd_, &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = select(0, &set, nullptr, nullptr, &tv);
#else
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, timeout_ms);
#endif
    if (r <= 0) return r < 0 ? -1 : 0;
    sockaddr_in a{};
    socklen_t n = sizeof a;
    int got = (int)recvfrom(fd_, (char*)buf, (int)cap, 0, (sockaddr*)&a, &n);
    if (got < 0) {
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAECONNRESET || e == WSAEMSGSIZE || e == WSAEWOULDBLOCK) return 0;
#endif
        return -1;
    }
    from.ip = ntohl(a.sin_addr.s_addr);
    from.port = ntohs(a.sin_port);
    return got;
}

bool resolve(const std::string& text, uint16_t default_port, Endpoint& out, std::string* err) {
    if (!winsock()) {
        if (err) *err = "Winsock not available";
        return false;
    }
    std::string host = text;
    uint16_t port = default_port;
    size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        std::string p = text.substr(colon + 1);
        char* end = nullptr;
        long v = std::strtol(p.c_str(), &end, 10);
        if (p.empty() || *end || v <= 0 || v > 65535) {
            if (err) *err = "bad port in '" + text + "'";
            return false;
        }
        port = (uint16_t)v;
    }
    while (!host.empty() && host.back() == ' ') host.pop_back();
    while (!host.empty() && host.front() == ' ') host.erase(0, 1);
    if (host.empty()) {
        if (err) *err = "no server address";
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (err) *err = "cannot find '" + host + "'";
        return false;
    }
    out.ip = ntohl(((sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(res);
    return true;
}

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace net
}  // namespace gp4u
