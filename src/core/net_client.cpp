#include "core/net_client.h"

namespace gp4u {
namespace net {

Client::Client(uint32_t token, std::string version, std::string room, std::string name)
    : ch_(token), version_(std::move(version)), room_(std::move(room)), name_(std::move(name)) {}

void Client::start(double now, std::vector<Bytes>& out) {
    started_ = now;
    Writer w;
    w.u8(kHello).u32(kProtocol).str(version_).str(room_).str(name_);
    msg(w, true, now, out);
}

void Client::request_list(const Guid& app, double now, std::vector<Bytes>& out) {
    Writer w;
    w.u8(kList).guid(app);
    msg(w, true, now, out);
}

uint32_t Client::create(const SessionDesc& d, double now, std::vector<Bytes>& out) {
    uint32_t req = next_req_++;
    Writer w;
    w.u8(kCreate).u32(req);
    write_desc(w, d);
    msg(w, true, now, out);
    return req;
}

uint32_t Client::join(const Guid& instance, double now, std::vector<Bytes>& out) {
    uint32_t req = next_req_++;
    Writer w;
    w.u8(kJoin).u32(req).guid(instance);
    msg(w, true, now, out);
    return req;
}

uint32_t Client::create_player(const std::string& short_name, const std::string& long_name, double now,
                               std::vector<Bytes>& out) {
    uint32_t req = next_req_++;
    Writer w;
    w.u8(kCreatePlayer).u32(req).str(short_name).str(long_name);
    msg(w, true, now, out);
    return req;
}

void Client::send(uint32_t to, bool guaranteed, const uint8_t* d, size_t n, double now, std::vector<Bytes>& out) {
    Writer w;
    w.u8(kSend).u32(to).u32(guaranteed ? kSendGuaranteed : 0).raw(d, n);
    msg(w, guaranteed, now, out);
}

void Client::set_desc(const SessionDesc& d, double now, std::vector<Bytes>& out) {
    Writer w;
    w.u8(kSetDesc);
    write_desc(w, d);
    msg(w, true, now, out);
}

void Client::leave(double now, std::vector<Bytes>& out) {
    Writer w;
    w.u8(kLeave);
    msg(w, true, now, out);
    in_session_ = false;
    my_id_ = 0;
    events_.clear();
}

void Client::bye(double now, std::vector<Bytes>& out) {
    Writer w;
    w.u8(kBye);
    msg(w, true, now, out);
    in_session_ = false;
    my_id_ = 0;
    events_.clear();
    state_ = State::Closed;
    closed_ = now;
}

bool Client::take_result(uint32_t req, Result& r) {
    auto it = results_.find(req);
    if (it == results_.end()) return false;
    r = it->second;
    results_.erase(it);
    return true;
}

void Client::lost(const std::string& why) {
    if (state_ != State::Connecting && state_ != State::Ready) return;
    state_ = State::Lost;
    problem_ = why;
    if (in_session_) {
        Event e;
        e.system = true;
        e.type = kSysSessionLost;
        events_.push_back(e);
        in_session_ = false;
    }
}

bool Client::receive(const uint8_t* d, size_t n, double now, std::vector<Bytes>& out) {
    std::vector<Bytes> in;
    if (!ch_.receive(d, n, now, in, out)) return false;
    for (auto& m : in) handle(m, now);
    return true;
}

void Client::handle(const Bytes& m, double now) {
    if (m.empty()) return;
    Reader r(m);
    switch (r.u8()) {
    case kWelcome: {
        uint32_t proto = r.u32();
        server_version_ = r.str();
        seen_as_ = r.str();
        if (proto == kProtocol && state_ == State::Connecting) state_ = State::Ready;
        break;
    }
    case kRefused:
        state_ = State::Refused;
        problem_ = r.str();
        break;
    case kSessions: {
        uint16_t n = r.u16();
        std::vector<SessionDesc> v;
        for (uint16_t i = 0; i < n && r.ok(); ++i) v.push_back(read_desc(r));
        if (!r.ok()) return;
        sessions_ = std::move(v);
        sessions_time_ = now;
        break;
    }
    case kResult: {
        uint32_t req = r.u32();
        Result res;
        res.hr = r.u32();
        // kCreate/kJoin answers carry a SessionDesc, kCreatePlayer an id: tell them apart
        // by what is left.
        if (res.hr == kOk && r.left() == 4) {
            res.id = r.u32();
            my_id_ = res.id;
        } else if (res.hr == kOk && r.left() > 4) {
            res.desc = read_desc(r);
            if (r.ok()) in_session_ = true, my_id_ = 0;
        }
        results_[req] = res;
        break;
    }
    case kData: {
        Event e;
        e.from = r.u32();
        e.to = r.u32();
        if (!r.ok() || !in_session_) return;
        e.data.assign(r.here(), r.here() + r.left());
        events_.push_back(std::move(e));
        break;
    }
    case kSystem: {
        Event e;
        e.system = true;
        e.type = r.u32();
        if (e.type == kSysCreatePlayer) {
            e.id = r.u32();
            e.players = r.u32();
            e.short_name = r.str();
            e.long_name = r.str();
        } else if (e.type == kSysDestroyPlayer) {
            e.id = r.u32();
            e.short_name = r.str();
            e.long_name = r.str();
        } else if (e.type == kSysSetSessionDesc) {
            e.desc = read_desc(r);
        }
        if (r.ok() && in_session_) events_.push_back(std::move(e));
        break;
    }
    default:
        break;
    }
}

void Client::poll(double now, std::vector<Bytes>& out) {
    if (state_ == State::Refused || state_ == State::Lost) return;
    if (state_ == State::Closed) {
        if (!idle() && now - closed_ < 3) ch_.poll(now, out);
        return;
    }
    if (state_ == State::Connecting && now - started_ > kConnectTimeout) {
        lost("no answer from the server");
        return;
    }
    if (state_ == State::Ready && (now - ch_.last_heard() > kSilence || ch_.failed(now))) {
        lost("the server stopped answering");
        return;
    }
    ch_.poll(now, out);
}

}  // namespace net
}  // namespace gp4u
