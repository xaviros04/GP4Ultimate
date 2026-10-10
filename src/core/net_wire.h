// Byte-level reading and writing for the GP4 Ultimate online protocol (little endian).
//
// Pure C++, no Windows: unit-tested in tests/test_online.cpp.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gp4u {
namespace net {

struct Guid {
    uint8_t b[16] = {};
    bool operator==(const Guid& o) const { return std::memcmp(b, o.b, 16) == 0; }
    bool operator!=(const Guid& o) const { return !(*this == o); }
    bool operator<(const Guid& o) const { return std::memcmp(b, o.b, 16) < 0; }
    bool null() const { return *this == Guid{}; }
};

class Writer {
public:
    std::vector<uint8_t> buf;
    Writer& u8(uint8_t v) { buf.push_back(v); return *this; }
    Writer& u16(uint16_t v) {
        uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        return raw(b, 2);
    }
    Writer& u32(uint32_t v) {
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        return raw(b, 4);
    }
    Writer& guid(const Guid& g) { return raw(g.b, 16); }
    // Strings: u16 length, bytes, capped at 1000 (names and versions only).
    Writer& str(const std::string& s) {
        size_t n = s.size() < 1000 ? s.size() : 1000;
        u16((uint16_t)n);
        return raw(s.data(), n);
    }
    Writer& bytes(const void* p, size_t n) { u32((uint32_t)n); return raw(p, n); }
    Writer& raw(const void* p, size_t n) {
        const uint8_t* c = (const uint8_t*)p;
        buf.insert(buf.end(), c, c + n);
        return *this;
    }
};

// Reads until the data runs out; after that every read returns 0/empty and ok() is false.
class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    explicit Reader(const std::vector<uint8_t>& v) : d_(v.data()), n_(v.size()) {}
    bool ok() const { return ok_; }
    size_t left() const { return ok_ ? n_ - p_ : 0; }
    const uint8_t* here() const { return d_ + p_; }
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint8_t b[2] = {}; raw(b, 2); return (uint16_t)(b[0] | b[1] << 8); }
    uint32_t u32() {
        uint8_t b[4] = {};
        raw(b, 4);
        return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    }
    Guid guid() { Guid g; raw(g.b, 16); return g; }
    std::string str() {
        uint16_t n = u16();
        if (!need(n)) return {};
        std::string s((const char*)d_ + p_, n);
        p_ += n;
        return s;
    }
    std::vector<uint8_t> bytes(size_t max) {
        uint32_t n = u32();
        if (n > max || !need(n)) { ok_ = false; return {}; }
        std::vector<uint8_t> v(d_ + p_, d_ + p_ + n);
        p_ += n;
        return v;
    }
    bool raw(void* out, size_t n) {
        if (!need(n)) { std::memset(out, 0, n); return false; }
        std::memcpy(out, d_ + p_, n);
        p_ += n;
        return true;
    }

private:
    bool need(size_t n) {
        if (ok_ && n_ - p_ >= n) return true;
        ok_ = false;
        return false;
    }
    const uint8_t* d_;
    size_t n_, p_ = 0;
    bool ok_ = true;
};

}  // namespace net
}  // namespace gp4u
