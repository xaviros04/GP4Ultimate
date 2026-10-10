#include "net/udp.h"   // winsock2.h before windows.h

#include "dplay_online.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <dplay.h>
#include <dplobby.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/net_client.h"
#include "core/net_server.h"
#include "core/version.h"
#include "log.h"

namespace gp4u {

namespace {

using net::Bytes;

OnlineConfig g_cfg;
bool g_active = false;
std::mutex g_quality_mtx;
bool g_quality_ok = false;
double g_quality_rtt = 0, g_quality_jitter = 0;

uint64_t mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

uint32_t new_token(const void* salt) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t v = mix((uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ (uintptr_t)salt ^ GetTickCount());
    uint32_t r = (uint32_t)v ^ (uint32_t)(v >> 32);
    return r ? r : 1;
}

// ---------------------------------------------------------------------------------------
// server = local: the server runs in this process, on its own thread, until GP4 exits.

class LocalServer {
public:
    bool start(uint16_t port, std::string* err) {
        if (!sock_.open(port, err)) return false;
        net::ServerConfig c;
        c.version = kVersion;
        server_ = net::Server(c);
        std::thread(&LocalServer::run, this).detach();   // never joined: it lives as long as GP4
        return true;
    }

private:
    void run() {
        std::vector<uint8_t> buf(net::kMaxDatagram + 64);
        std::vector<net::Server::Out> out;
        for (;;) {
            net::Endpoint from;
            int n = sock_.recv(from, buf.data(), buf.size(), 5);
            double now = net::seconds();
            while (n > 0) {
                server_.receive(from, buf.data(), (size_t)n, now, out);
                n = sock_.recv(from, buf.data(), buf.size(), 0);
            }
            server_.poll(now, out);
            for (auto& o : out) sock_.send(o.to, o.data.data(), o.data.size());
            out.clear();
            for (auto& l : server_.take_log()) logf("server: %s", l.c_str());
        }
    }
    net::Udp sock_;
    net::Server server_;
};

std::mutex g_local_mtx;
LocalServer* g_local = nullptr;
std::string g_local_error;

bool ensure_local_server(std::string* err) {
    std::lock_guard<std::mutex> lk(g_local_mtx);
    if (g_local) return true;
    if (!g_local_error.empty()) {
        if (err) *err = g_local_error;
        return false;
    }
    auto* s = new LocalServer;
    std::string e;
    if (!s->start(g_cfg.port, &e)) {
        delete s;
        g_local_error = "cannot run the server here: " + e;
        if (err) *err = g_local_error;
        logf("net: %s", g_local_error.c_str());
        return false;
    }
    g_local = s;
    logf("net: server running in this GP4 on UDP port %u: the other players set [net] server = <your address>:%u "
         "(open that UDP port in your router)", g_cfg.port, g_cfg.port);
    return true;
}

// ---------------------------------------------------------------------------------------
// The conversation with the server: a socket, the protocol state (core/net_client.h) and a
// thread that keeps it moving (receives, resends, keep-alives).

class Link {
public:
    using State = net::Client::State;

    ~Link() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (client_ && client_->state() == State::Ready) {
                std::vector<Bytes> out;
                client_->bye(net::seconds(), out);
                transmit(out);
            }
        }
        // Give the goodbye a moment to be acknowledged, then stop.
        for (int i = 0; i < 40 && running_; ++i) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (!client_ || client_->idle()) break;
            }
            Sleep(5);
        }
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    bool start(std::string* err) {
        net::Endpoint server;
        std::string where = g_cfg.server;
        if (where == "local") {
            if (!ensure_local_server(err)) return false;
            server.ip = 0x7f000001;
            server.port = g_cfg.port;
        } else if (!net::resolve(where, net::kDefaultPort, server, err)) {
            return false;
        }
        if (!sock_.open(0, err)) return false;
        server_ = server;
        client_.reset(new net::Client(new_token(this), kVersion, g_cfg.room, ""));
        std::vector<Bytes> out;
        client_->start(net::seconds(), out);
        transmit(out);
        logf("net: connecting to the server %s%s", server.str().c_str(),
             g_cfg.room.empty() ? "" : (" (room '" + g_cfg.room + "')").c_str());
        running_ = true;
        thread_ = std::thread(&Link::run, this);
        return true;
    }

    // Runs f(client, out, now) under the lock and sends what it produced.
    template <typename F>
    auto call(F f) -> decltype(f(std::declval<net::Client&>(), std::declval<std::vector<Bytes>&>(), 0.0)) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<Bytes> out;
        struct Send {
            Link* l;
            std::vector<Bytes>& out;
            ~Send() { l->transmit(out); }
        } s{this, out};
        return f(*client_, out, net::seconds());
    }

    State state() {
        std::lock_guard<std::mutex> lk(mtx_);
        return client_->state();
    }
    double ended_at() const { return ended_at_; }
    std::string problem() {
        std::lock_guard<std::mutex> lk(mtx_);
        return client_->problem();
    }

    bool wait_ready(double timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds((int)(timeout * 1000)),
                     [&] { return client_->state() != State::Connecting; });
        return client_->state() == State::Ready;
    }

    // The answer to a request; false (r.hr = timeout or no connection) if none came.
    bool wait_result(uint32_t req, net::Client::Result& r, double timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        bool got = false;
        cv_.wait_for(lk, std::chrono::milliseconds((int)(timeout * 1000)), [&] {
            got = client_->take_result(req, r);
            return got || client_->state() != State::Ready;
        });
        if (!got) r.hr = client_->state() == State::Ready ? net::kErrTimeout : net::kErrNoConnection;
        return got;
    }

    // Receive and GetMessageCount work on the event queue under the lock.
    template <typename F>
    auto with_events(F f) -> decltype(f(std::declval<std::deque<net::Event>&>())) {
        std::lock_guard<std::mutex> lk(mtx_);
        return f(client_->events());
    }

    void set_event(HANDLE h) {
        std::lock_guard<std::mutex> lk(mtx_);
        event_ = h;
    }
    double rtt() {
        std::lock_guard<std::mutex> lk(mtx_);
        return client_->rtt();
    }
    double jitter() {
        std::lock_guard<std::mutex> lk(mtx_);
        return client_->jitter();
    }

private:
    void transmit(std::vector<Bytes>& out) {
        for (auto& d : out) sock_.send(server_, d.data(), d.size());
        out.clear();
    }

    void run() {
        std::vector<uint8_t> buf(net::kMaxDatagram + 64);
        State seen = State::Connecting;
        double last_rtt_log = 0;
        while (!stop_) {
            net::Endpoint from;
            int n = sock_.recv(from, buf.data(), buf.size(), 2);
            std::lock_guard<std::mutex> lk(mtx_);
            double now = net::seconds();
            std::vector<Bytes> out;
            size_t before = client_->events().size();
            while (n > 0) {
                if (from == server_) client_->receive(buf.data(), (size_t)n, now, out);
                n = sock_.recv(from, buf.data(), buf.size(), 0);
            }
            client_->poll(now, out);
            transmit(out);
            if (client_->events().size() > before && event_) SetEvent(event_);
            State st = client_->state();
            if (st != seen) {
                if (st == State::Ready)
                    logf("net: connected to the server (GP4 Ultimate server %s); it sees us as %s",
                         client_->server_version().c_str(), client_->seen_as().c_str());
                else if (st == State::Refused || st == State::Lost) {
                    logf("net: server %s: %s", server_.str().c_str(), client_->problem().c_str());
                    ended_at_ = now;
                }
                seen = st;
            }
            if (st == State::Ready && client_->rtt() > 0 && now - last_rtt_log > 60) {
                logf("net: round trip to the server %.0f ms", client_->rtt() * 1000);
                last_rtt_log = now;
            }
            cv_.notify_all();
            if (st == State::Lost || st == State::Refused || (st == State::Closed && client_->idle())) break;
        }
        running_ = false;
    }

    net::Udp sock_;
    net::Endpoint server_;
    std::unique_ptr<net::Client> client_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, running_{false};
    HANDLE event_ = nullptr;
    double ended_at_ = 0;
};

// ---------------------------------------------------------------------------------------
// Conversions between DirectPlay's structures and the protocol's.

net::Guid to_guid(const GUID& g) {
    net::Guid r;
    std::memcpy(r.b, &g, 16);
    return r;
}
GUID from_guid(const net::Guid& g) {
    GUID r;
    std::memcpy(&r, g.b, 16);
    return r;
}

net::SessionDesc from_dp(const DPSESSIONDESC2& d) {
    net::SessionDesc s;
    s.instance = to_guid(d.guidInstance);
    s.application = to_guid(d.guidApplication);
    s.flags = d.dwFlags;
    s.max_players = d.dwMaxPlayers;
    s.players = d.dwCurrentPlayers;
    if (d.lpszSessionNameA) s.name = d.lpszSessionNameA;
    s.user[0] = d.dwUser1;
    s.user[1] = d.dwUser2;
    s.user[2] = d.dwUser3;
    s.user[3] = d.dwUser4;
    return s;
}

// name: storage for the name (GP4 copies at most 31 characters safely, 0x5078b0).
void to_dp(const net::SessionDesc& s, DPSESSIONDESC2& d, std::string& name) {
    std::memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = s.flags;
    d.guidInstance = from_guid(s.instance);
    d.guidApplication = from_guid(s.application);
    d.dwMaxPlayers = s.max_players;
    d.dwCurrentPlayers = s.players;
    name = s.name.substr(0, net::kMaxSessionName);
    d.lpszSessionNameA = (LPSTR)name.c_str();
    d.dwUser1 = s.user[0];
    d.dwUser2 = s.user[1];
    d.dwUser3 = s.user[2];
    d.dwUser4 = s.user[3];
}

// A message as GP4's Receive gets it. System messages hold pointers to their strings,
// which must point into the caller's buffer: `fix` lists (pointer offset, string offset).
struct Image {
    Bytes bytes;
    std::vector<std::pair<size_t, size_t>> fix;

    size_t add_string(const std::string& s) {
        size_t off = bytes.size();
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
        return off;
    }
    template <typename T>
    void put(const T& v) {
        bytes.resize(sizeof v);
        std::memcpy(bytes.data(), &v, sizeof v);
    }
    void copy_to(void* dst) const {
        std::memcpy(dst, bytes.data(), bytes.size());
        for (auto& f : fix) {
            char* p = (char*)dst + f.second;
            std::memcpy((char*)dst + f.first, &p, sizeof p);
        }
    }
};

template <typename T, typename M>
size_t offset_in(const T& base, const M& member) {
    return (size_t)((const char*)&member - (const char*)&base);
}

Image image_of(const net::Event& e) {
    Image im;
    if (!e.system) {
        im.bytes = e.data;
        return im;
    }
    switch (e.type) {
    case net::kSysCreatePlayer: {
        DPMSG_CREATEPLAYERORGROUP m;
        std::memset(&m, 0, sizeof m);
        m.dwType = DPSYS_CREATEPLAYERORGROUP;
        m.dwPlayerType = DPPLAYERTYPE_PLAYER;
        m.dpId = e.id;
        m.dwCurrentPlayers = e.players;
        m.dpnName.dwSize = sizeof(DPNAME);
        im.put(m);
        im.fix.push_back({offset_in(m, m.dpnName.lpszShortNameA), im.add_string(e.short_name)});
        if (!e.long_name.empty()) im.fix.push_back({offset_in(m, m.dpnName.lpszLongNameA), im.add_string(e.long_name)});
        break;
    }
    case net::kSysDestroyPlayer: {
        DPMSG_DESTROYPLAYERORGROUP m;
        std::memset(&m, 0, sizeof m);
        m.dwType = DPSYS_DESTROYPLAYERORGROUP;
        m.dwPlayerType = DPPLAYERTYPE_PLAYER;
        m.dpId = e.id;
        m.dpnName.dwSize = sizeof(DPNAME);
        im.put(m);
        im.fix.push_back({offset_in(m, m.dpnName.lpszShortNameA), im.add_string(e.short_name)});
        if (!e.long_name.empty()) im.fix.push_back({offset_in(m, m.dpnName.lpszLongNameA), im.add_string(e.long_name)});
        break;
    }
    case net::kSysSetSessionDesc: {
        DPMSG_SETSESSIONDESC m;
        std::string name;
        m.dwType = DPSYS_SETSESSIONDESC;
        to_dp(e.desc, m.dpDesc, name);
        m.dpDesc.lpszSessionNameA = nullptr;
        im.put(m);
        im.fix.push_back({offset_in(m, m.dpDesc.lpszSessionNameA), im.add_string(name)});
        break;
    }
    default: {   // DPSYS_HOST, DPSYS_SESSIONLOST: just the type
        DWORD t = e.type;
        im.put(t);
        break;
    }
    }
    return im;
}

const GUID& tcpip() { return DPSPGUID_TCPIP; }

// Walks a DirectPlay address ({GUID type, DWORD size, data}...).
template <typename F>
void walk_address(const void* addr, DWORD size, F f) {
    const uint8_t* p = (const uint8_t*)addr;
    DWORD off = 0;
    while (p && off + sizeof(GUID) + 4 <= size) {
        GUID type;
        DWORD n;
        std::memcpy(&type, p + off, sizeof type);
        std::memcpy(&n, p + off + sizeof(GUID), 4);
        if (off + sizeof(GUID) + 4 + n > size) break;
        if (!f(type, n, p + off + sizeof(GUID) + 4)) break;
        off += sizeof(GUID) + 4 + n;
    }
}

// Size of an address whose length is not given: from its DPAID_TotalSize element if it
// starts with one, else just the first element.
DWORD address_size(const void* addr) {
    if (!addr) return 0;
    GUID first;
    DWORD n;
    std::memcpy(&first, addr, sizeof first);
    std::memcpy(&n, (const uint8_t*)addr + sizeof(GUID), 4);
    DWORD total = sizeof(GUID) + 4 + n;
    if (first == DPAID_TotalSize && n == 4) std::memcpy(&total, (const uint8_t*)addr + sizeof(GUID) + 4, 4);
    return total;
}

Bytes compound_address(const DPCOMPOUNDADDRESSELEMENT* el, DWORD count) {
    Bytes b;
    auto add = [&](const GUID& g, DWORD n, const void* d) {
        size_t o = b.size();
        b.resize(o + sizeof(GUID) + 4 + n);
        std::memcpy(&b[o], &g, sizeof g);
        std::memcpy(&b[o + sizeof g], &n, 4);
        if (n) std::memcpy(&b[o + sizeof g + 4], d, n);
    };
    DWORD total = sizeof(GUID) + 8;
    for (DWORD i = 0; i < count; ++i) total += sizeof(GUID) + 4 + el[i].dwDataSize;
    add(DPAID_TotalSize, 4, &total);
    for (DWORD i = 0; i < count; ++i) add(el[i].guidDataType, el[i].dwDataSize, el[i].lpData);
    return b;
}

// ---------------------------------------------------------------------------------------
// IDirectPlay4A.

class OnlineDirectPlay final : public IDirectPlay4 {
public:
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectPlay4A || riid == IID_IDirectPlay3A || riid == IID_IDirectPlay2A) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)++refs_; }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = --refs_;
        if (r == 0) {
            if (open_) Close();
            delete this;
        }
        return (ULONG)r;
    }

    // What GP4 uses.
    STDMETHOD(EnumConnections)(LPCGUID, LPDPENUMCONNECTIONSCALLBACK cb, LPVOID ctx, DWORD) override {
        if (!cb) return DPERR_INVALIDPARAMS;
        DPCOMPOUNDADDRESSELEMENT el;
        el.guidDataType = DPAID_ServiceProvider;
        el.dwDataSize = sizeof(GUID);
        el.lpData = (LPVOID)&tcpip();
        Bytes addr = compound_address(&el, 1);
        DPNAME name;
        std::memset(&name, 0, sizeof name);
        name.dwSize = sizeof name;
        name.lpszShortNameA = (LPSTR) "GP4 Ultimate server (TCP/IP)";
        cb(&tcpip(), addr.data(), (DWORD)addr.size(), &name, 0, ctx);
        return DP_OK;
    }

    STDMETHOD(InitializeConnection)(LPVOID conn, DWORD) override {
        if (initialized_) return DPERR_ALREADYINITIALIZED;
        if (!conn) return DPERR_INVALIDPARAMS;
        bool tcp = false;
        walk_address(conn, address_size(conn), [&](const GUID& type, DWORD n, const uint8_t* d) {
            if (type == DPAID_ServiceProvider && n == sizeof(GUID)) tcp = std::memcmp(d, &tcpip(), sizeof(GUID)) == 0;
            return true;
        });
        if (!tcp) return DPERR_UNAVAILABLE;   // only "TCP/IP" exists here
        initialized_ = true;
        return DP_OK;
    }

    STDMETHOD(EnumSessions)(LPDPSESSIONDESC2 filter, DWORD timeout, LPDPENUMSESSIONSCALLBACK2 cb, LPVOID ctx,
                            DWORD flags) override {
        if (!initialized_) return DPERR_UNINITIALIZED;
        if (flags & DPENUMSESSIONS_STOPASYNC) return DP_OK;
        if (!cb) return DPERR_INVALIDPARAMS;
        net::Guid app = filter ? to_guid(filter->guidApplication) : net::Guid{};
        std::string why;
        std::vector<net::SessionDesc> list;
        if (ensure_link(&why)) {
            double now = net::seconds();
            if (!(flags & DPENUMSESSIONS_ASYNC)) {
                // Synchronous: wait for a fresh list.
                double wait = timeout ? timeout / 1000.0 : 2.0;
                if (link_->wait_ready(wait)) {
                    link_->call([&](net::Client& c, std::vector<Bytes>& o, double t) { c.request_list(app, t, o); return 0; });
                    for (double end = now + wait; net::seconds() < end;) {
                        double got = link_->call([&](net::Client& c, std::vector<Bytes>&, double) { return c.sessions_time(); });
                        if (got >= now) break;
                        Sleep(5);
                    }
                }
            } else if (now - list_asked_ >= 1.0) {
                // Asynchronous (GP4 polls every frame): ask again once a second.
                link_->call([&](net::Client& c, std::vector<Bytes>& o, double t) {
                    if (c.state() == net::Client::State::Ready) c.request_list(app, t, o), list_asked_ = now;
                    return 0;
                });
            }
            list = link_->call([&](net::Client& c, std::vector<Bytes>&, double) { return c.sessions(); });
        }
        bool all = (flags & DPENUMSESSIONS_ALL) != 0;
        DWORD to = timeout;
        for (auto& s : list) {
            if (!app.null() && s.application != app) continue;
            if (!all && !net::joinable(s)) continue;
            DPSESSIONDESC2 d;
            std::string name;
            to_dp(s, d, name);
            if (!cb(&d, &to, 0, ctx)) return DP_OK;
        }
        cb(nullptr, &to, DPESC_TIMEDOUT, ctx);
        return DP_OK;
    }

    STDMETHOD(Open)(LPDPSESSIONDESC2 d, DWORD flags) override {
        if (!initialized_) return DPERR_UNINITIALIZED;
        if (!d) return DPERR_INVALIDPARAMS;
        if (open_) return DPERR_ALREADYINITIALIZED;
        std::string why;
        if (!ensure_link(&why)) {
            logf("net: no server: %s", why.c_str());
            return DPERR_NOCONNECTION;
        }
        if (!link_->wait_ready(net::Client::kConnectTimeout)) {
            logf("net: no server: %s", link_->problem().c_str());
            return DPERR_NOCONNECTION;
        }
        bool create = (flags & DPOPEN_CREATE) != 0;
        net::SessionDesc want = from_dp(*d);
        uint32_t req = link_->call([&](net::Client& c, std::vector<Bytes>& o, double t) {
            return create ? c.create(want, t, o) : c.join(want.instance, t, o);
        });
        net::Client::Result r;
        link_->wait_result(req, r, 10);
        if (r.hr != DP_OK) return (HRESULT)r.hr;
        open_ = true;
        desc_ = r.desc;
        {
            std::lock_guard<std::mutex> lk(g_quality_mtx);
            g_quality_rtt = link_->rtt() * 1000;
            g_quality_jitter = link_->jitter() * 1000;
            g_quality_ok = g_quality_rtt > 0;
        }
        if (create) d->guidInstance = from_guid(desc_.instance);
        return DP_OK;
    }

    STDMETHOD(CreatePlayer)(LPDPID id, LPDPNAME name, HANDLE ev, LPVOID, DWORD, DWORD) override {
        if (!id) return DPERR_INVALIDPARAMS;
        if (!open_) return DPERR_INVALIDOBJECT;
        std::string s = name && name->lpszShortNameA ? name->lpszShortNameA : "";
        std::string l = name && name->lpszLongNameA ? name->lpszLongNameA : "";
        uint32_t req = link_->call([&](net::Client& c, std::vector<Bytes>& o, double t) { return c.create_player(s, l, t, o); });
        net::Client::Result r;
        link_->wait_result(req, r, 10);
        if (r.hr != DP_OK) return (HRESULT)r.hr;
        me_ = r.id;
        *id = me_;
        link_->set_event(ev);
        return DP_OK;
    }

    STDMETHOD(Receive)(LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size) override {
        if (!size) return DPERR_INVALIDPARAMS;
        if (!link_) return DPERR_NOMESSAGES;
        return link_->with_events([&](std::deque<net::Event>& q) -> HRESULT {
            for (auto it = q.begin(); it != q.end(); ++it) {
                DPID f = it->system ? DPID_SYSMSG : it->from, t = it->system ? me_ : it->to;
                if ((flags & DPRECEIVE_TOPLAYER) && to && *to != t) continue;
                if ((flags & DPRECEIVE_FROMPLAYER) && from && *from != f) continue;
                Image im = image_of(*it);
                if (!data || *size < im.bytes.size()) {
                    *size = (DWORD)im.bytes.size();
                    return DPERR_BUFFERTOOSMALL;
                }
                im.copy_to(data);
                *size = (DWORD)im.bytes.size();
                if (from) *from = f;
                if (to) *to = t;
                if (!(flags & DPRECEIVE_PEEK)) q.erase(it);
                return DP_OK;
            }
            return DPERR_NOMESSAGES;
        });
    }

    STDMETHOD(Send)(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size) override {
        if (!open_ || !link_) return DPERR_NOCONNECTION;
        if (!me_ || from != me_) return DPERR_INVALIDPLAYER;
        if (size > net::kMaxGameMessage) return DPERR_SENDTOOBIG;
        if (size && !data) return DPERR_INVALIDPARAMS;
        bool guaranteed = (flags & DPSEND_GUARANTEED) != 0;
        link_->call([&](net::Client& c, std::vector<Bytes>& o, double t) {
            c.send(to, guaranteed, (const uint8_t*)data, size, t, o);
            return 0;
        });
        return DP_OK;
    }
    STDMETHOD(SendEx)(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size, DWORD, DWORD, LPVOID, LPDWORD) override {
        return Send(from, to, flags, data, size);
    }

    STDMETHOD(GetMessageCount)(DPID, LPDWORD n) override {
        if (!n) return DPERR_INVALIDPARAMS;
        *n = link_ ? link_->with_events([](std::deque<net::Event>& q) { return (DWORD)q.size(); }) : 0;
        return DP_OK;
    }

    STDMETHOD(SetSessionDesc)(LPDPSESSIONDESC2 d, DWORD) override {
        if (!d) return DPERR_INVALIDPARAMS;
        if (!open_) return DPERR_NOSESSIONS;
        net::SessionDesc s = from_dp(*d);
        s.instance = desc_.instance;
        desc_.flags = s.flags;
        desc_.max_players = s.max_players;
        desc_.name = s.name;
        std::memcpy(desc_.user, s.user, sizeof s.user);
        link_->call([&](net::Client& c, std::vector<Bytes>& o, double t) {
            c.set_desc(s, t, o);
            return 0;
        });
        return DP_OK;
    }

    STDMETHOD(GetSessionDesc)(LPVOID data, LPDWORD size) override {
        if (!size) return DPERR_INVALIDPARAMS;
        if (!open_) return DPERR_NOSESSIONS;
        DPSESSIONDESC2 d;
        std::string name;
        to_dp(desc_, d, name);
        DWORD need = (DWORD)(sizeof d + name.size() + 1);
        if (!data || *size < need) {
            *size = need;
            return DPERR_BUFFERTOOSMALL;
        }
        char* s = (char*)data + sizeof d;
        std::memcpy(s, name.c_str(), name.size() + 1);
        d.lpszSessionNameA = s;
        std::memcpy(data, &d, sizeof d);
        *size = need;
        return DP_OK;
    }

    STDMETHOD(GetCaps)(LPDPCAPS caps, DWORD) override {
        if (!caps || caps->dwSize < sizeof(DPCAPS)) return DPERR_INVALIDPARAMS;
        DWORD sz = caps->dwSize;
        std::memset(caps, 0, sizeof(DPCAPS));
        caps->dwSize = sz;
        caps->dwFlags = DPCAPS_GUARANTEEDSUPPORTED | DPCAPS_GUARANTEEDOPTIMIZED | (open_ ? 0 : 0);
        caps->dwMaxBufferSize = net::kMaxGameMessage;
        caps->dwMaxPlayers = desc_.max_players ? desc_.max_players : 22;
        caps->dwHundredBaud = 100000;
        caps->dwLatency = link_ ? (DWORD)(link_->rtt() * 500) : 0;
        caps->dwMaxLocalPlayers = 1;
        caps->dwHeaderLength = (DWORD)net::kHeaderSize;
        caps->dwTimeout = 5000;
        return DP_OK;
    }
    STDMETHOD(GetPlayerCaps)(DPID, LPDPCAPS caps, DWORD f) override { return GetCaps(caps, f); }

    STDMETHOD(Close)() override {
        if (open_ && link_)
            link_->call([&](net::Client& c, std::vector<Bytes>& o, double t) {
                c.leave(t, o);
                return 0;
            });
        open_ = false;
        me_ = 0;
        desc_ = net::SessionDesc();
        return DP_OK;
    }

    STDMETHOD(DestroyPlayer)(DPID) override { return DP_OK; }

    // Not used by GP4.
    STDMETHOD(AddPlayerToGroup)(DPID, DPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(CreateGroup)(LPDPID, LPDPNAME, LPVOID, DWORD, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(DeletePlayerFromGroup)(DPID, DPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(DestroyGroup)(DPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(EnumGroupPlayers)(DPID, LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(EnumGroups)(LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(EnumPlayers)(LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetGroupData)(DPID, LPVOID, LPDWORD, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetGroupName)(DPID, LPVOID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetPlayerAddress)(DPID, LPVOID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetPlayerData)(DPID, LPVOID, LPDWORD, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetPlayerName)(DPID, LPVOID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(Initialize)(LPGUID) override { return DPERR_ALREADYINITIALIZED; }
    STDMETHOD(SetGroupData)(DPID, LPVOID, DWORD, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SetGroupName)(DPID, LPDPNAME, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SetPlayerData)(DPID, LPVOID, DWORD, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SetPlayerName)(DPID, LPDPNAME, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(AddGroupToGroup)(DPID, DPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(CreateGroupInGroup)(DPID, LPDPID, LPDPNAME, LPVOID, DWORD, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(DeleteGroupFromGroup)(DPID, DPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(EnumGroupsInGroup)(DPID, LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetGroupConnectionSettings)(DWORD, DPID, LPVOID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SecureOpen)(LPCDPSESSIONDESC2, DWORD, LPCDPSECURITYDESC, LPCDPCREDENTIALS) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SendChatMessage)(DPID, DPID, DWORD, LPDPCHAT) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SetGroupConnectionSettings)(DWORD, DPID, LPDPLCONNECTION) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(StartSession)(DWORD, DPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetGroupFlags)(DPID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetGroupParent)(DPID, LPDPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetPlayerAccount)(DPID, DWORD, LPVOID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetPlayerFlags)(DPID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetGroupOwner)(DPID, LPDPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SetGroupOwner)(DPID, DPID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetMessageQueue)(DPID, DPID, DWORD, LPDWORD n, LPDWORD bytes) override {
        DWORD c = 0;
        GetMessageCount(0, &c);
        if (n) *n = c;
        if (bytes) *bytes = 0;
        return DP_OK;
    }
    STDMETHOD(CancelMessage)(DWORD, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(CancelPriority)(DWORD, DWORD, DWORD) override { return DPERR_UNSUPPORTED; }

private:
    ~OnlineDirectPlay() = default;

    // The server conversation starts with the first list or open (GP4 also creates DirectPlay
    // objects only to probe the connection types; those never connect).
    bool ensure_link(std::string* why) {
        if (link_) {
            auto st = link_->state();
            if (st == net::Client::State::Ready || st == net::Client::State::Connecting) return true;
            if (open_ || net::seconds() - link_->ended_at() < 3) {   // retry every few seconds
                if (why) *why = link_->problem();
                return false;
            }
            link_.reset();
        }
        std::unique_ptr<Link> l(new Link);
        if (!l->start(why)) {
            logf("net: cannot use the server '%s': %s", g_cfg.server.c_str(), why ? why->c_str() : "");
            return false;
        }
        link_ = std::move(l);
        list_asked_ = -1e9;
        return true;
    }

    std::atomic<LONG> refs_{1};
    bool initialized_ = false, open_ = false;
    DPID me_ = 0;
    net::SessionDesc desc_;
    std::unique_ptr<Link> link_;
    double list_asked_ = -1e9;
};

// ---------------------------------------------------------------------------------------
// IDirectPlayLobby3A: GP4 only builds addresses with it (and lists modems).

class OnlineLobby final : public IDirectPlayLobby3 {
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectPlayLobby3A || riid == IID_IDirectPlayLobby2A ||
            riid == IID_IDirectPlayLobbyA) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)++refs_; }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = --refs_;
        if (r == 0) delete this;
        return (ULONG)r;
    }

    STDMETHOD(CreateCompoundAddress)(LPCDPCOMPOUNDADDRESSELEMENT el, DWORD count, LPVOID out, LPDWORD size) override {
        if (!size || (count && !el)) return DPERR_INVALIDPARAMS;
        Bytes b = compound_address(el, count);
        if (!out || *size < b.size()) {
            *size = (DWORD)b.size();
            return DPERR_BUFFERTOOSMALL;
        }
        std::memcpy(out, b.data(), b.size());
        *size = (DWORD)b.size();
        return DP_OK;
    }
    STDMETHOD(CreateAddress)(REFGUID sp, REFGUID type, LPCVOID data, DWORD n, LPVOID out, LPDWORD size) override {
        DPCOMPOUNDADDRESSELEMENT el[2];
        el[0].guidDataType = DPAID_ServiceProvider;
        el[0].dwDataSize = sizeof(GUID);
        el[0].lpData = (LPVOID)&sp;
        el[1].guidDataType = type;
        el[1].dwDataSize = n;
        el[1].lpData = (LPVOID)data;
        return CreateCompoundAddress(el, 2, out, size);
    }
    STDMETHOD(EnumAddress)(LPDPENUMADDRESSCALLBACK cb, LPCVOID addr, DWORD size, LPVOID ctx) override {
        if (!cb || !addr) return DPERR_INVALIDPARAMS;
        walk_address(addr, size, [&](const GUID& type, DWORD n, const uint8_t* d) { return cb(type, n, d, ctx) != FALSE; });
        return DP_OK;
    }

    STDMETHOD(Connect)(DWORD, LPDIRECTPLAY2*, IUnknown*) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(EnumAddressTypes)(LPDPLENUMADDRESSTYPESCALLBACK, REFGUID, LPVOID, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(EnumLocalApplications)(LPDPLENUMLOCALAPPLICATIONSCALLBACK, LPVOID, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(GetConnectionSettings)(DWORD, LPVOID, LPDWORD) override { return DPERR_NOTLOBBIED; }
    STDMETHOD(ReceiveLobbyMessage)(DWORD, DWORD, LPDWORD, LPVOID, LPDWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(RunApplication)(DWORD, LPDWORD, LPDPLCONNECTION, HANDLE) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SendLobbyMessage)(DWORD, DWORD, LPVOID, DWORD) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SetConnectionSettings)(DWORD, DWORD, LPDPLCONNECTION) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(SetLobbyMessageEvent)(DWORD, DWORD, HANDLE) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(ConnectEx)(DWORD, REFIID, LPVOID*, IUnknown*) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(RegisterApplication)(DWORD, LPDPAPPLICATIONDESC) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(UnregisterApplication)(DWORD, REFGUID) override { return DPERR_UNSUPPORTED; }
    STDMETHOD(WaitForConnectionSettings)(DWORD) override { return DPERR_UNSUPPORTED; }

private:
    ~OnlineLobby() = default;
    std::atomic<LONG> refs_{1};
};

}  // namespace

void online_setup(const OnlineConfig& c) {
    g_cfg = c;
    g_active = !c.server.empty();
}

bool online_active() { return g_active; }

bool online_open_quality(double& rtt_ms, double& jitter_ms) {
    std::lock_guard<std::mutex> lk(g_quality_mtx);
    rtt_ms = g_quality_rtt;
    jitter_ms = g_quality_jitter;
    return g_active && g_quality_ok;
}

std::string online_summary() {
    if (!g_active) return "off";
    std::string s = g_cfg.server == "local" ? "server in this GP4, UDP port " + std::to_string(g_cfg.port) : g_cfg.server;
    if (!g_cfg.room.empty()) s += ", room '" + g_cfg.room + "'";
    return s;
}

HRESULT online_create_dplay(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    auto* dp = new OnlineDirectPlay;
    HRESULT hr = dp->QueryInterface(riid, out);
    dp->Release();
    return hr;
}

HRESULT online_create_lobby(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    auto* l = new OnlineLobby;
    HRESULT hr = l->QueryInterface(riid, out);
    l->Release();
    return hr;
}

}  // namespace gp4u
