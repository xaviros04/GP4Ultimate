// gp4ultimate-server: the GP4 Ultimate online server (Windows and Linux).
//
// Players point their gp4ultimate.ini at it ([net] server = address:port) and play over
// the internet without a VPN or Windows' DirectPlay. It keeps the list of games and relays
// the players' messages; one player's GP4 still runs the race. Only this machine needs an
// open port (UDP, 24004 by default). See docs/ONLINE.md.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/net_server.h"
#include "core/version.h"
#include "net/udp.h"

using namespace gp4u;
using namespace gp4u::net;

static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static void say(const std::string& s) {
    std::time_t t = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::printf("%s  %s\n", b, s.c_str());
    std::fflush(stdout);
}

static void usage() {
    std::printf(
        "gp4ultimate-server %s - online server for GP4 Ultimate\n\n"
        "  --port N          UDP port to listen on (default %u)\n"
        "  --max-games N     games at the same time (default 64)\n"
        "  --max-clients N   players connected at the same time (default 256)\n"
        "  --max-players N   players per game (default 22)\n\n"
        "Players set in gp4ultimate.ini:  [net] server = <this machine's address>:<port>\n"
        "Open (forward) the UDP port on this machine's router/firewall.\n",
        kVersion, kDefaultPort);
}

int main(int argc, char** argv) {
    ServerConfig cfg;
    cfg.version = kVersion;
    unsigned port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto num = [&](unsigned lo, unsigned hi) -> unsigned {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            char* end = nullptr;
            unsigned long v = std::strtoul(argv[++i], &end, 10);
            if (*end || v < lo || v > hi) {
                std::fprintf(stderr, "%s: expected a number from %u to %u\n", a.c_str(), lo, hi);
                std::exit(2);
            }
            return (unsigned)v;
        };
        if (a == "--port") port = num(1, 65535);
        else if (a == "--max-games") cfg.max_sessions = num(1, 10000);
        else if (a == "--max-clients") cfg.max_clients = num(1, 10000);
        else if (a == "--max-players") cfg.max_players = num(2, 64);
        else if (a == "-h" || a == "--help" || a == "/?") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n\n", a.c_str()); usage(); return 2; }
    }

    Udp sock;
    std::string err;
    if (!sock.open((uint16_t)port, &err)) {
        std::fprintf(stderr, "cannot listen on UDP %s (is another server running?)\n", err.c_str());
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    say(std::string("GP4 Ultimate server ") + kVersion + " listening on UDP port " + std::to_string(port) +
        " (Ctrl+C to stop)");

    Server server(cfg);
    std::vector<uint8_t> buf(kMaxDatagram + 64);
    std::vector<Server::Out> out;
    double last_status = seconds();
    size_t shown_clients = 0, shown_games = 0;
    while (!g_stop) {
        Endpoint from;
        int n = sock.recv(from, buf.data(), buf.size(), 5);
        double now = seconds();
        while (n > 0) {   // everything that is waiting, then the timers
            server.receive(from, buf.data(), (size_t)n, now, out);
            n = sock.recv(from, buf.data(), buf.size(), 0);
        }
        server.poll(now, out);
        for (auto& o : out) sock.send(o.to, o.data.data(), o.data.size());
        out.clear();
        for (auto& l : server.take_log()) say(l);
        if (now - last_status >= 300 && (server.clients() != shown_clients || server.sessions() != shown_games)) {
            shown_clients = server.clients();
            shown_games = server.sessions();
            say(std::to_string(shown_clients) + " players connected, " + std::to_string(shown_games) + " games");
            last_status = now;
        }
    }
    say("stopped");
    return 0;
}
