// Network diagnostics for GP4's DirectPlay traffic (gp4net.log). Pure C++: the DLL feeds
// it every Send/Receive; it returns one summary line per second plus a hex dump of the
// first packets of each kind, so a real online session tells us rates, sizes, who waits
// for whom and the packet layouts before the mod replaces the transport.
//
// GP4's packets (static reading, docs/GP4-FINDINGS.md "Red"): menu/lobby messages start
// with a 32-bit header 0x80000000 | type << 24 | frame; race packets do not have bit 31.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gp4u {

class NetStats {
public:
    explicit NetStats(int dumps_per_kind = 3) : dumps_per_kind_(dumps_per_kind) {}

    void on_send(double t, uint32_t to, uint32_t flags, const uint8_t* data, uint32_t size);
    void on_recv(double t, uint32_t from, const uint8_t* data, uint32_t size);
    void on_empty_poll(double t) { (void)t; ++cur_.empty_polls; }

    // Lines to log now: hex dumps queued since the last call, and the summary of the last
    // period once `period` seconds have passed (only if there was any traffic).
    std::vector<std::string> poll(double t, double period = 1.0);

    static std::string kind_of(const uint8_t* data, uint32_t size);   // "menu3", "race", ...
    static std::string hex(const uint8_t* data, uint32_t size, uint32_t max_bytes = 48);

private:
    struct Peer {
        unsigned packets = 0;
        unsigned long long bytes = 0;
        double last = -1, gap_sum = 0, gap_max = 0;
        unsigned gaps = 0;
    };
    struct Period {
        unsigned sent = 0, sent_guaranteed = 0;
        unsigned long long sent_bytes = 0;
        unsigned long long empty_polls = 0;
        std::map<uint32_t, Peer> from;
        std::map<std::string, unsigned> kinds;
    };
    void count_kind(const std::string& k, const char* dir, const uint8_t* data, uint32_t size, uint32_t peer);

    int dumps_per_kind_;
    double start_ = -1;
    Period cur_;
    std::map<uint32_t, double> last_seen_;   // across periods, for gaps
    std::map<std::string, int> dumped_;
    std::vector<std::string> pending_;
};

}  // namespace gp4u
