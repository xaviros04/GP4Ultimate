#include "ffb_engine.h"
#include "physics_hook.h"

#include "device_manager.h"  // windows.h first

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/car_data.h"
#include "core/car_locator.h"
#include "core/gp4_addresses.h"
#include "core/ffb_model.h"
#include "device_manager.h"
#include "log.h"
#include "mod.h"

namespace gp4u {

FfbEngine& FfbEngine::get() {
    static FfbEngine e;
    return e;
}

bool FfbEngine::start() {
    if (running_) return true;
    if (!DeviceManager::get().ffb_device()) return false;
    running_ = true;
    th_ = std::thread([this] { run(); });
    logf("ffb: engine started (mode %s)", ffb_mode_name(mod().ffb_mode));
    return true;
}

void FfbEngine::stop() {
    if (!running_) return;
    running_ = false;
    if (th_.joinable()) th_.join();
    logf("ffb: engine stopped");
}

void FfbEngine::on_device_reset() { reset_ = true; }

namespace {

bool read_self(uint32_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n, &got) && got == n;
}

struct Region { uint32_t base, size; bool image; };

// `skip_base/skip_size`: our own scan buffer, which holds copies of whatever it last read
// (including car arrays) and must never be mistaken for the live data.
std::vector<Region> scan_regions(bool heap, uint64_t max_total, uintptr_t skip_base, size_t skip_size) {
    std::vector<Region> out;
    // Writable sections of GP4.exe (.data/.bss): static game state lives here.
    auto* mz = (const IMAGE_DOS_HEADER*)GetModuleHandleA(nullptr);
    auto* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)mz + mz->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uint32_t base = (uint32_t)(uintptr_t)mz + s->VirtualAddress;
        uint32_t size = std::max<uint32_t>(s->Misc.VirtualSize, s->SizeOfRawData);
        out.push_back({base, size, true});
    }
    if (!heap) return out;
    uint64_t total = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    while (a < 0x7FFF0000 && VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) == sizeof mbi) {
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        bool rw = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
        uintptr_t rb = (uintptr_t)mbi.BaseAddress;
        bool own = rb < skip_base + skip_size && skip_base < rb + mbi.RegionSize;
        if (!own && mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && mbi.RegionSize >= 0x41C * 6) {
            if (total + mbi.RegionSize > max_total) break;
            total += mbi.RegionSize;
            out.push_back({(uint32_t)(uintptr_t)mbi.BaseAddress, (uint32_t)mbi.RegionSize, false});
        }
        if (next <= a) break;
        a = next;
    }
    return out;
}

struct Candidate {
    uint32_t addr = 0;
    uint32_t array_count = 0;
    SteerCorrelator raw{200, 0.8, 0.15};
    SteerCorrelator steer{200, 0.8, 0.15};
    bool confirmed = false, rejected = false;
};

class Telemetry {
public:
    void configure(const Ini& ini) {
        fixed_ = (uint32_t)ini.get_int("telemetry", "car_address", 0);
        // Default: GP4's own pointer to the player's car (verified in GP4.exe, see
        // core/gp4_addresses.h). car_pointer = 0 falls back to scanning memory.
        // Only on the exact build: another GP4.exe would have different addresses.
        known_ = is_known_build();
        pointer_ = (uint32_t)ini.get_int("telemetry", "car_pointer", known_ ? gp4::kPlayerCarPtr : 0);
        if (pointer_ == gp4::kPlayerCarPtr && known_) table_ = gp4::kCarArray;
        logf("telemetry: GP4.exe build %s", known_ ? "recognised (known addresses)" : "NOT recognised - scanning instead");
        respect_pause_ = ini.get_bool("telemetry", "respect_game_pause", true);
        for (int i = 0; i < 3; ++i) pause_flags_[i] = known_ ? gp4::kForceZeroFlags[i] : 0;
        std::string pf = ini.get("telemetry", "pause_flags", "");
        if (!pf.empty()) {
            int n = 0;
            size_t pos = 0;
            while (n < 3 && pos <= pf.size()) {
                size_t comma = pf.find(',', pos);
                long long v;
                if (parse_int(pf.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos), v))
                    pause_flags_[n++] = (uint32_t)v;
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            for (; n < 3; ++n) pause_flags_[n] = 0;
        }
        aligning_ = Ini::lower(ini.get("ffb", "game_signal", "total")) == "aligning";
        aligning_addr_ = (uint32_t)ini.get_int("telemetry", "aligning_address", known_ ? gp4::kAligningTerm : 0);
        if (aligning_ && !aligning_addr_) {
            logf("telemetry: game_signal = aligning needs the known build or aligning_address; using total");
            aligning_ = false;
        }
        wheel_record_ = (uint32_t)ini.get_int("telemetry", "wheel_record", known_ ? gp4::kWheelRecord : 0);
        stride_ = (uint32_t)ini.get_int("telemetry", "car_stride", car::kSize);
        min_run_ = (uint32_t)ini.get_int("telemetry", "min_run", 6);
        heap_ = ini.get_bool("telemetry", "scan_heap", true);
        require_confirm_ = ini.get_bool("telemetry", "require_confirm", false);
        confirm_threshold_ = std::clamp(ini.get_double("telemetry", "confirm_threshold", 0.8), 0.5, 0.99);
        if (stride_ < car::kSize) stride_ = car::kSize;
        if (fixed_) logf("telemetry: fixed car address 0x%08x", fixed_);
        else if (pointer_) logf("telemetry: car pointer at 0x%08x", pointer_);
        else logf("telemetry: car_pointer = 0 -> scanning memory for the car table");
        logf("telemetry: game signal = %s%s", aligning_ ? "aligning term" : "total (+0x5E)",
             respect_pause_ ? ", zero force when GP4 does" : "");
    }

    // Returns true and fills `s` when a usable player car record was read.
    bool sample(double now, double steer_input, car::Sample& s) {
        now_ = now;
        if (respect_pause_) {
            // GP4 itself sends no force while any of these is set (pause/menus/replay).
            paused_ = false;
            for (uint32_t f : pause_flags_) {
                uint8_t b = 0;
                if (f && read_self(f, &b, 1) && b) paused_ = true;
            }
            if (paused_) return false;
        }
        uint32_t addr = current_address(now);
        last_addr_ = addr;
        if (!addr) return false;
        uint8_t buf[car::kSize];
        // A fixed address or GP4's own player pointer is trusted as is: the plausibility
        // check only exists to tell car records from random memory while scanning. (Found
        // in the first real run: GP4's pointer was right but the check rejected the record.)
        bool trusted = fixed_ || pointer_;
        if (!read_self(addr, buf, sizeof buf)) return false;
        if (const char* why = car::implausible_reason(buf)) {
            if (now >= next_diag_) {
                next_diag_ = now + 30.0;
                logf("telemetry: record 0x%08x fails '%s' (%s): id=%02x gears=%u weight=%u gear=%u team=%u"
                     " compound=%u wings=%u/%u speed=%d ff=%d rpm=%u ffscale=%u", addr, why,
                     trusted ? "used anyway: trusted address" : "rejected", buf[car::kId], buf[car::kNumGears],
                     (unsigned)car::rd<uint16_t>(buf, car::kWeight), buf[car::kCurrentGear], buf[car::kTeamIndex],
                     buf[car::kTyreCompound], buf[car::kFrontWing], buf[car::kRearWing],
                     (int)car::rd<int32_t>(buf, car::kSpeed), (int)(int16_t)car::rd<uint16_t>(buf, car::kForceFeedback),
                     (unsigned)car::rd<uint16_t>(buf, car::kEngineRpm), buf[car::kFfScaling]);
            }
            if (!trusted) {
                drop(now, "record no longer looks like the player car");
                return false;
            }
        } else if (!trusted && !(buf[car::kId] & 0x80)) {
            drop(now, "record no longer looks like the player car");
            return false;
        }
        s = car::decode(buf);
        if (aligning_addr_) {
            // Same scaling as GP4 applies to the total: * ffScaling / 100, clamp +-10000.
            int32_t v = 0;
            if (read_self(aligning_addr_, &v, 4)) {
                long long x = (long long)v * s.ff_scaling / 100;
                s.game_align = (int16_t)std::clamp<long long>(x, -10000, 10000);
                s.has_align = true;
            } else if (aligning_) {
                return false;
            }
            if (aligning_) s.game_ffb = s.game_align;
        }
        if (wheel_record_) {
            // Per-car wheel record: surface, slides (core/gp4_addresses.h).
            uint8_t slot = buf[gp4::kCarSlot];
            uint8_t rec[gp4::kWheelRecordStride];
            if (slot < 22 && read_self(wheel_record_ + slot * gp4::kWheelRecordStride, rec, sizeof rec)) {
                for (int w = 0; w < 4; ++w) {
                    std::memcpy(&s.surface[w], rec + gp4::kWrSurface + 4 * w, 4);
                    std::memcpy(&s.slide_lat[w], rec + gp4::kWrSlideLat + 4 * w, 4);
                    std::memcpy(&s.slide_long[w], rec + gp4::kWrSlideLong + 4 * w, 4);
                }
                s.has_wheels = true;
            }
        }
        PhysSnap ps;
        if (physics_hook_installed() && physics_snapshot(ps)) {
            for (int w = 0; w < 4; ++w) s.fx[w] = ps.fx[w];
            s.g_long = ps.g_long;
            s.has_forces = true;
        }
        if (!fixed_ && !pointer_) {
            validate(steer_input, s);
            if (cands_.empty()) return false;   // every candidate was discarded
            if (require_confirm_ && !cands_[active_].confirmed) return false;
        }
        return true;
    }

    std::string state() const {
        char b[160];
        if (paused_) std::snprintf(b, sizeof b, "game paused/idle");
        else if (fixed_) std::snprintf(b, sizeof b, "fixed address 0x%08x", fixed_);
        else if (pointer_) std::snprintf(b, sizeof b, last_addr_ ? "car at 0x%08x via pointer" : "no player car (pointer 0)", last_addr_);
        else if (cands_.empty()) std::snprintf(b, sizeof b, "searching");
        else std::snprintf(b, sizeof b, "car at 0x%08x (%s)", cands_[active_].addr,
                           cands_[active_].confirmed ? "confirmed by steering" : "unconfirmed");
        return b;
    }

private:
    uint32_t current_address(double now) {
        if (fixed_) return fixed_;
        if (pointer_) {
            uint32_t p = 0;
            if (!read_self(pointer_, &p, 4)) return 0;
            if (table_ && (p < table_ || (p - table_) % car::kSize || (p - table_) / car::kSize >= gp4::kNumCars))
                return 0;   // not a record of GP4's car table
            return p;
        }
        if (scanner_.joinable() && scan_done_) {
            scanner_.join();
            adopt_scan();
        }
        if (cands_.empty() && !scanner_.joinable() && now >= next_scan_) {
            next_scan_ = now + 2.0;
            scan_done_ = false;
            scanner_ = std::thread([this] { scan(); scan_done_ = true; });
        }
        return cands_.empty() ? 0 : cands_[active_].addr;
    }

public:
    ~Telemetry() {
        if (scanner_.joinable()) scanner_.join();
    }

private:
    // Runs on the scanner thread: reads memory only, touches nothing but scan_result_.
    void scan() {
        const uint32_t chunk = 8u << 20, overlap = stride_ * 32;
        size_t cap = chunk + overlap;
        uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) return;
        std::vector<Region> regions = scan_regions(heap_, 512ull << 20, (uintptr_t)buf, cap);
        std::vector<CarArrayCandidate> found;
        std::vector<bool> from_image;
        for (const Region& r : regions) {
            for (uint32_t off = 0; off < r.size; off += chunk) {
                uint32_t n = std::min<uint32_t>(r.size - off, chunk + overlap);
                if (!read_self(r.base + off, buf, n)) continue;
                for (auto& c : find_car_arrays(buf, n, r.base + off, stride_, min_run_)) {
                    bool dup = false;
                    for (auto& f : found) dup |= f.base == c.base || (c.base > f.base && (c.base - f.base) % stride_ == 0 && c.base < f.base + f.count * stride_);
                    if (!dup) { found.push_back(c); from_image.push_back(r.image); }
                }
            }
        }
        VirtualFree(buf, 0, MEM_RELEASE);
        // Arrays inside GP4.exe's own data sections first (static game state), then by size.
        std::vector<size_t> order(found.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (from_image[a] != from_image[b]) return (bool)from_image[a];
            return found[a].count > found[b].count;
        });
        std::vector<CarArrayCandidate> sorted;
        for (size_t i : order) sorted.push_back(found[i]);
        found.swap(sorted);
        scan_result_ = std::move(found);
        scan_regions_ = (unsigned)regions.size();
    }

    // Engine thread, after the scanner finished.
    void adopt_scan() {
        std::vector<CarArrayCandidate> found = std::move(scan_result_);
        cands_.clear();
        active_ = 0;
        for (auto& f : found)
            for (uint32_t p : f.players) {
                bool skip = false;
                for (auto& r : rejected_) skip |= r.addr == p && r.until > now_;
                if (skip) continue;
                Candidate c;
                c.raw = SteerCorrelator(200, confirm_threshold_, 0.15);
                c.steer = SteerCorrelator(200, confirm_threshold_, 0.15);
                c.addr = p;
                c.array_count = f.count;
                cands_.push_back(c);
            }
        if (!cands_.empty() || ++quiet_scans_ % 15 == 1)
            logf("telemetry: scan of %u regions: %u car arrays, %u player candidates%s", scan_regions_,
                 (unsigned)found.size(), (unsigned)cands_.size(),
                 cands_.empty() ? "" : (" - using 0x" + hex(cands_[0].addr)).c_str());
    }

    void validate(double steer_input, const car::Sample& s) {
        if (++tick_ % 10) return;  // 50 Hz at the default 500 Hz engine rate
        for (auto& c : cands_) {
            if (c.confirmed || c.rejected) continue;
            uint8_t buf[car::kSize];
            if (!read_self(c.addr, buf, sizeof buf)) continue;
            car::Sample cs = car::decode(buf);
            c.raw.add(steer_input, (double)cs.steer_raw);
            c.steer.add(steer_input, (double)cs.steer);
            if (c.raw.confirmed() || c.steer.confirmed()) {
                c.confirmed = true;
                logf("telemetry: 0x%08x confirmed as the player car (r_raw=%.2f r_steer=%.2f)", c.addr, c.raw.r(), c.steer.r());
                active_ = (size_t)(&c - &cands_[0]);
            } else if (c.raw.rejected() && c.steer.rejected()) {
                c.rejected = true;
                logf("telemetry: 0x%08x does not follow the steering (r=%.2f/%.2f), discarded", c.addr, c.raw.r(), c.steer.r());
            }
        }
        (void)s;
        if (!cands_[active_].confirmed && cands_[active_].rejected) {
            for (size_t i = 0; i < cands_.size(); ++i)
                if (!cands_[i].rejected) { active_ = i; return; }
            for (auto& c : cands_) rejected_.push_back({c.addr, now_ + 30.0});
            cands_.clear();
            active_ = 0;
        }
    }

    void drop(double now, const char* why) {
        if (fixed_ || pointer_ || cands_.empty()) return;
        logf("telemetry: dropping 0x%08x: %s", cands_[active_].addr, why);
        cands_.clear();
        active_ = 0;
        next_scan_ = now + 1.0;
    }

    static std::string hex(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "%08x", v);
        return b;
    }

    uint32_t fixed_ = 0, pointer_ = 0, stride_ = car::kSize, min_run_ = 6;
    bool heap_ = true, require_confirm_ = false;
    bool respect_pause_ = true, paused_ = false, aligning_ = false;
    uint32_t pause_flags_[3] = {};
    uint32_t aligning_addr_ = 0, last_addr_ = 0, table_ = 0, wheel_record_ = 0;
    bool known_ = false;

public:
    // Code signatures of this build, read from our own process: the car-index helper
    // (0x4083a0: imul eax,eax,41Ch / add eax,0x66fef0 / ret) and the FF read
    // (0x51e467: mov esi,[0x66d820]).
    static bool is_known_build() {
        static const uint8_t car_index[] = {0x69, 0xC0, 0x1C, 0x04, 0x00, 0x00, 0x05, 0xF0, 0xFE, 0x66, 0x00, 0xC3};
        static const uint8_t ff_read[] = {0x8B, 0x35, 0x20, 0xD8, 0x66, 0x00};
        uint8_t a[sizeof car_index], b[sizeof ff_read];
        return read_self(0x004083A0, a, sizeof a) && !std::memcmp(a, car_index, sizeof a) &&
               read_self(0x0051E467, b, sizeof b) && !std::memcmp(b, ff_read, sizeof b);
    }

private:
    double confirm_threshold_ = 0.8;
    double next_diag_ = 0;
    std::vector<Candidate> cands_;
    std::thread scanner_;
    std::atomic<bool> scan_done_{false};
    std::vector<CarArrayCandidate> scan_result_;
    unsigned scan_regions_ = 0;
    struct Rejected { uint32_t addr; double until; };
    std::vector<Rejected> rejected_;   // retried after 30 s: a new session can reuse an address
    double now_ = 0;
    size_t active_ = 0;
    double next_scan_ = 0;
    unsigned tick_ = 0, quiet_scans_ = 0;
};

bool is_lost(HRESULT hr) {
    return hr == (HRESULT)DIERR_INPUTLOST || hr == (HRESULT)DIERR_NOTACQUIRED || hr == (HRESULT)DIERR_NOTEXCLUSIVEACQUIRED ||
           hr == (HRESULT)DIERR_NOTDOWNLOADED || hr == (HRESULT)DIERR_UNPLUGGED;
}

}  // namespace

void FfbEngine::run() {
    const Ini& ini = mod().ini;
    FfbParams params;
    params.load(ini);
    FfbModel model;
    model.configure(params);
    Telemetry tel;
    tel.configure(ini);
    double rate = std::clamp(ini.get_double("ffb", "rate_hz", 500.0), 60.0, 1000.0);
    bool csv_on = ini.get_bool("ffb", "log_csv", false);
    int csv_every = (int)std::max<long long>(1, ini.get_int("ffb", "log_every", 5));
    FILE* csv = nullptr;
    if (csv_on) {
        csv = std::fopen((mod().dir + "\\gp4ultimate_ffb.csv").c_str(), "w");
        if (csv)
            std::fprintf(csv, "t,valid,speed_mps,game_ffb,steer,gear,rpm,susp0,susp1,susp2,susp3,yaw_raw,heading_raw,"
                              "c_game,c_model,c_road,c_shift,c_impact,c_damping,c_friction,c_spring,yaw_rate,slip_front,output,game_align,model_raw,game_hz,surf_fl,surf_fr,surf_rl,surf_rr,slide_fl,slide_fr,slide_long_fl,slide_long_fr,c_texture,fx_fl,fx_fr,g_long,c_brake\n");
    }

    DeviceManager& dm = DeviceManager::get();
    IDirectInputEffect* effect = nullptr;
    DICONSTANTFORCE cf{0};
    DWORD axes[1] = {joystate2_axis_offset(dm.ffb_axis())};
    LONG dirs[1] = {0};
    DIEFFECT eff{};
    eff.dwSize = sizeof eff;
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 1;
    eff.rgdwAxes = axes;
    eff.rglDirection = dirs;
    eff.cbTypeSpecificParams = sizeof cf;
    eff.lpvTypeSpecificParams = &cf;

    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    LARGE_INTEGER freq, t0, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    prev = t0;
    const double period = 1.0 / rate;
    double retry_at = 0, next_status = 5.0;
    LONG last_mag = 0x7FFFFFFF;
    unsigned long ticks = 0;
    uint32_t effect_gen = 0;
    double next_check = 0;
    unsigned recreated = 0, restarted = 0, errors = 0;
    // Problems repeat at 500 Hz: log the first few, then every 100th.
    auto worth_logging = [](unsigned n) { return n <= 10 || n % 100 == 0; };

    while (running_) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) / freq.QuadPart;
        if (dt < period) {
            double left = period - dt;
            if (left > 0.0015) Sleep(1);
            else SwitchToThread();
            continue;
        }
        prev = now;
        double t = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        ++ticks;

        double steer;
        {
            std::lock_guard<std::mutex> lk(dm.mutex());
            steer = dm.steer_locked();
        }
        car::Sample s;
        FfbInput in;
        in.steer = steer;
        if (tel.sample(t, steer, s)) {
            in.car_valid = true;
            in.speed_mps = s.speed_mps;
            in.yaw_raw = s.yaw_raw;
            in.vel_heading_raw = s.vel_heading_raw;
            in.game_ffb = s.game_ffb;
            in.game_align = s.game_align;
            in.has_align = s.has_align;
            in.has_wheels = s.has_wheels;
            for (int w = 0; w < 4; ++w) {
                in.surface[w] = s.surface[w];
                in.slide_lat[w] = s.slide_lat[w];
                in.slide_long[w] = s.slide_long[w];
                in.fx[w] = s.fx[w];
            }
            in.has_forces = s.has_forces;
            in.gear = s.gear;
            for (int i = 0; i < 4; ++i) in.susp[i] = s.susp[i];
        }
        double out = model.step(in, dt);

        std::unique_lock<std::mutex> lk(dm.mutex());
        if (reset_.exchange(false) && effect) {
            effect->Release();
            effect = nullptr;
        }
        if (effect && dm.ffb_generation_locked() != effect_gen) {
            // The wheel was re-acquired (window focus change, display mode switch...). That
            // reset the device and silently stopped our effect: the "dead wheel" seen in-game.
            effect->Release();
            effect = nullptr;
            ++recreated;
            if (worth_logging(recreated))
                logf("ffb: wheel re-acquired (window focus change?) - effect re-created (#%u)", recreated);
        }
        if (!effect && t >= retry_at) {
            if (dm.ffb_ready_locked()) {
                cf.lMagnitude = (LONG)std::lround(std::clamp(out, -1.0, 1.0) * DI_FFNOMINALMAX);
                HRESULT hr = dm.ffb_device()->dev->CreateEffect(GUID_ConstantForce, &eff, &effect, nullptr);
                if (SUCCEEDED(hr) && effect) {
                    HRESULT hs = effect->Start(1, 0);
                    effect_gen = dm.ffb_generation_locked();
                    last_mag = cf.lMagnitude;
                    next_check = t + 0.1;
                    if (FAILED(hs)) logf("ffb: Start failed 0x%08lx", (unsigned long)hs);
                    if (recreated == 0) logf("ffb: constant-force effect created");
                } else {
                    effect = nullptr;
                    logf("ffb: CreateEffect failed 0x%08lx, retrying", (unsigned long)hr);
                    retry_at = t + 1.0;
                }
            } else {
                retry_at = t + 0.25;   // window not focused yet, or wheel not exclusive
            }
        }
        if (effect) {
            LONG mag = (LONG)std::lround(std::clamp(out, -1.0, 1.0) * DI_FFNOMINALMAX);
            if (mag != last_mag) {
                cf.lMagnitude = mag;
                HRESULT hr = effect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
                if (is_lost(hr) || hr == DI_DOWNLOADSKIPPED) {
                    // Not (exclusively) acquired any more: the next steering read re-acquires,
                    // which bumps the generation and re-creates the effect.
                    dm.mark_ffb_unacquired_locked();
                    if (is_lost(hr)) {
                        effect->Release();
                        effect = nullptr;
                        retry_at = t + 0.05;
                    }
                } else if (FAILED(hr)) {
                    if (worth_logging(++errors)) logf("ffb: SetParameters failed 0x%08lx (#%u)", (unsigned long)hr, errors);
                } else {
                    last_mag = mag;
                }
            }
        }
        if (effect && t >= next_check) {
            // Belt and braces: anything else that stops the effect (a driver tool, another
            // program, a reset we did not see) is caught within 100 ms.
            next_check = t + 0.1;
            DWORD st = 0;
            if (SUCCEEDED(effect->GetEffectStatus(&st)) && !(st & DIEGES_PLAYING)) {
                HRESULT hs = effect->Start(1, 0);
                ++restarted;
                if (worth_logging(restarted))
                    logf("ffb: effect found stopped - restarted (#%u, 0x%08lx)", restarted, (unsigned long)hs);
            }
        }

        lk.unlock();

        if (csv && ticks % csv_every == 0) {
            const FfbComponents& c = model.last();
            std::fprintf(csv, "%.4f,%d,%.3f,%d,%.4f,%d,%u,%ld,%ld,%ld,%ld,%ld,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.1f,%d,%d,%d,%d,%ld,%ld,%ld,%ld,%.4f,%ld,%ld,%ld,%.4f\n",
                         t, in.car_valid ? 1 : 0, in.speed_mps, in.game_ffb, steer, in.gear, (unsigned)s.rpm,
                         (long)in.susp[0], (long)in.susp[1], (long)in.susp[2], (long)in.susp[3], (long)in.yaw_raw,
                         (unsigned)in.vel_heading_raw, c.game, c.model, c.road, c.shift, c.impact, c.damping, c.friction,
                         c.spring, c.yaw_rate, c.slip_front, c.output, (int)in.game_align, c.model_raw, c.game_hz, in.surface[2], in.surface[3], in.surface[0],
                         in.surface[1], (long)in.slide_lat[2], (long)in.slide_lat[3], (long)s.slide_long[2],
                         (long)s.slide_long[3], c.texture, (long)s.fx[2], (long)s.fx[3], (long)s.g_long, c.brake);
        }
        if (t >= next_status) {
            next_status = t + 5.0;
            logf("ffb: %.0f Hz, telemetry %s, speed %.1f m/s, game_ffb %d, out %+.2f, effect %s (re-created %u, restarted %u)",
                 ticks / 5.0, tel.state().c_str(), in.speed_mps, in.game_ffb, out, effect ? "on" : "off", recreated,
                 restarted);
            ticks = 0;
        }
    }

    {
        std::lock_guard<std::mutex> lk(dm.mutex());
        if (effect) {
            cf.lMagnitude = 0;
            effect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
            effect->Stop();
            effect->Release();
        }
    }
    if (csv) std::fclose(csv);
    timeEndPeriod(1);
}

}  // namespace gp4u
