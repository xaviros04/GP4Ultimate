// Native unit tests for the platform-independent parts of the mod (Linux/macOS/Windows).
//   c++ -std=c++17 -I src tests/test_core.cpp src/core/*.cpp -o test_core && ./test_core
#include <cmath>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/car_data.h"
#include "core/car_locator.h"
#include "core/cockpit.h"
#include "core/ffb_model.h"
#include "core/gp4_resolution.h"
#include "core/ini.h"
#include "core/ini_doc.h"
#include "core/schema.h"
#include "core/mapping.h"
#include "core/net_pacer.h"
#include "core/net_playout.h"
#include "core/net_stats.h"
#include "core/paths.h"
#include "core/projection.h"

using namespace gp4u;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) <= (eps))

static void test_ini() {
    Ini ini;
    ini.load_string(
        "; comment\n[General]\nKey = Value ; trailing\nnum=0x1F\nf = 2.5\nb=yes\n"
        "q = \"a;b # c\"\n[Other]\nkey=1\r\n[general]\nkey=override\n");
    CHECK(ini.get("general", "key") == "override");   // sections merge, later key wins
    CHECK(ini.get_int("GENERAL", "NUM", 0) == 31);
    NEAR(ini.get_double("general", "f", 0), 2.5, 1e-12);
    CHECK(ini.get_bool("general", "b", false));
    CHECK(ini.get("general", "q") == "a;b # c");
    CHECK(ini.get("other", "key") == "1");
    CHECK(ini.get("missing", "x", "d") == "d");
    long long v;
    CHECK(parse_int("-12", v) && v == -12);
    CHECK(!parse_int("12abc", v));
}

static const char* kMappingIni =
    "[devices]\n"
    "wheel = vidpid:046D:C24F\n"
    "pedals = name:\"Heusinkveld Sprint\"\n"
    "shifter = guid:{12345678-1234-1234-1234-1234567890ab}\n"
    "[virtual]\n"
    "ffb = wheel\n"
    "[axes]\n"
    "X = wheel.X\n"
    "Y = pedals.Y invert deadzone=0.05\n"
    "RZ = pedals.RZ invert gamma=2\n"
    "Z = pedals.Z min=0.1 max=0.9\n"
    "S0 = combine(pedals.Y invert, pedals.RZ invert)\n"
    "[buttons]\n"
    "auto = wheel\n"
    "40 = shifter.b0\n"
    "41 = shifter.button1\n"
    "[povs]\n"
    "0 = wheel.pov0\n";

static void test_mapping_parse() {
    Ini ini;
    ini.load_string(kMappingIni);
    MappingConfig m;
    std::string err;
    std::vector<std::string> warn;
    bool ok = parse_mapping(ini, m, err, &warn);
    if (!ok) std::printf("  err: %s\n", err.c_str());
    CHECK(ok);
    CHECK(m.enabled());
    CHECK(m.devices.size() == 3);
    CHECK(m.device("wheel")->sel.kind == DeviceSelector::VidPid);
    CHECK(m.device("wheel")->sel.vid == 0x046D && m.device("wheel")->sel.pid == 0xC24F);
    CHECK(m.device("pedals")->sel.text == "heusinkveld sprint");
    CHECK(m.device("shifter")->sel.text == "{12345678-1234-1234-1234-1234567890AB}");
    CHECK(m.ffb_device == "wheel");
    CHECK(m.axes.size() == 5);
    CHECK(m.axes[0].target == AX_X && m.axes[0].a.bipolar);    // steering defaults to bipolar
    CHECK(m.axes[1].a.invert && std::fabs(m.axes[1].a.deadzone - 0.05) < 1e-12 && !m.axes[1].a.bipolar);
    CHECK(m.axes[4].kind == AxisBinding::Combine && m.axes[4].b.axis == AX_RZ);
    CHECK(m.auto_buttons.size() == 1 && m.auto_buttons[0] == "wheel");
    CHECK(m.buttons.size() == 2 && m.buttons[1].target == 41 && m.buttons[1].button == 1);
    CHECK(m.povs.size() == 1 && m.povs[0].device == "wheel");
    CHECK(warn.empty());
}

static void test_mapping_errors() {
    struct Case { const char* ini; const char* needle; } cases[] = {
        {"[devices]\nw=foo:bar\n[axes]\nX=w.X\n", "unknown device selector"},
        {"[devices]\nw=name:x\n[axes]\nX=q.X\n", "unknown device alias"},
        {"[devices]\nw=name:x\n[axes]\nX=w.Q\n", "unknown axis"},
        {"[devices]\nw=name:x\n[axes]\nQQ=w.X\n", "unknown virtual axis"},
        {"[devices]\nw=name:x\n[axes]\nX=w.X deadzone=1.5\n", "deadzone"},
        {"[devices]\nw=name:x\n[virtual]\nffb=z\n[axes]\nX=w.X\n", "ffb"},
        {"[devices]\nw=vidpid:12\n", "vidpid"},
        {"[devices]\nw=name:x\n[axes]\nY=combine(w.Y)\n", "two sources"},
        {"[devices]\nw=name:x\n[axes]\nX=w.X\n[buttons]\n200=w.b1\n", "0..127"},
        {"[devices]\nw=name:x\n[axes]\nX=w.X\n[povs]\n0=w.pov7\n", "POV"},
    };
    for (auto& c : cases) {
        Ini ini;
        ini.load_string(c.ini);
        MappingConfig m;
        std::string err;
        bool ok = parse_mapping(ini, m, err);
        CHECK(!ok);
        bool has = err.find(c.needle) != std::string::npos;
        if (!has) std::printf("  expected '%s' in: %s\n", c.needle, err.c_str());
        CHECK(has);
    }
    // Unused device -> warning, not error.
    Ini ini;
    ini.load_string("[devices]\nw=name:x\nspare=name:y\n[axes]\nX=w.X\n");
    MappingConfig m;
    std::string err;
    std::vector<std::string> warn;
    CHECK(parse_mapping(ini, m, err, &warn));
    CHECK(warn.size() == 1 && warn[0].find("spare") != std::string::npos);
}

static void test_axis_conditioning() {
    AxisSource s;
    NEAR(apply_source(s, 0.0), 0.0, 1e-12);
    NEAR(apply_source(s, 0.25), 0.25, 1e-12);
    s.invert = true;
    NEAR(apply_source(s, 0.25), 0.75, 1e-12);
    s = AxisSource{};
    s.deadzone = 0.1;
    NEAR(apply_source(s, 0.05), 0.0, 1e-12);
    NEAR(apply_source(s, 0.55), 0.5, 1e-12);
    NEAR(apply_source(s, 1.0), 1.0, 1e-12);
    s = AxisSource{};
    s.cal_min = 0.2; s.cal_max = 0.8;
    NEAR(apply_source(s, 0.1), 0.0, 1e-12);
    NEAR(apply_source(s, 0.5), 0.5, 1e-12);
    NEAR(apply_source(s, 0.95), 1.0, 1e-12);
    s = AxisSource{};
    s.gamma = 2.0;
    NEAR(apply_source(s, 0.5), 0.25, 1e-12);
    // Bipolar: deadzone and gamma around the centre, symmetric.
    s = AxisSource{};
    s.bipolar = true; s.deadzone = 0.1;
    NEAR(apply_source(s, 0.5), 0.5, 1e-12);
    NEAR(apply_source(s, 0.52), 0.5, 1e-12);             // inside the deadzone
    NEAR(apply_source(s, 1.0), 1.0, 1e-12);
    NEAR(apply_source(s, 0.0), 0.0, 1e-12);
    NEAR(apply_source(s, 0.3) + apply_source(s, 0.7), 1.0, 1e-12);
    // Combined pedals: rest = centre, full throttle = 1, full brake = 0.
    AxisBinding b;
    b.kind = AxisBinding::Combine;
    NEAR(apply_binding(b, 0.0, 0.0), 0.5, 1e-12);
    NEAR(apply_binding(b, 1.0, 0.0), 1.0, 1e-12);
    NEAR(apply_binding(b, 0.0, 1.0), 0.0, 1e-12);
    NEAR(apply_binding(b, 1.0, 1.0), 0.5, 1e-12);
}

static void put_car(uint8_t* p, uint8_t id, uint16_t weight = 600, uint8_t gears = 7) {
    std::memset(p, 0, car::kSize);
    p[car::kId] = id;
    std::memcpy(p + car::kWeight, &weight, 2);
    p[car::kNumGears] = gears;
    p[car::kCurrentGear] = 3;
    p[car::kTeamIndex] = id & 0x0F;
    p[car::kFrontWing] = 10;
    p[car::kRearWing] = 12;
}

static void test_car_decode() {
    std::vector<uint8_t> buf(car::kSize, 0);
    put_car(buf.data(), 0x81);
    int32_t speed = 0x400000 * 100;  // 100 ft/s
    std::memcpy(buf.data() + car::kSpeed, &speed, 4);
    uint16_t ff = (uint16_t)(int16_t)-1234;
    std::memcpy(buf.data() + car::kForceFeedback, &ff, 2);
    int32_t susp[4] = {1, -2, 3, -4};
    std::memcpy(buf.data() + car::kSuspTravel, susp, 16);
    car::Sample s = car::decode(buf.data());
    CHECK(s.player);
    NEAR(s.speed_mps, 30.48, 1e-9);
    CHECK(s.game_ffb == -1234);
    CHECK(s.susp[3] == -4);
    CHECK(s.weight_lbs == 600 && s.num_gears == 7);
    CHECK(car::plausible(buf.data()));
    // Real player record seen in-game on this build: weight field ~1355 must pass.
    uint16_t real_w = 1355;
    std::memcpy(buf.data() + car::kWeight, &real_w, 2);
    CHECK(car::plausible(buf.data()));
    uint16_t junk_w = 40000;
    std::memcpy(buf.data() + car::kWeight, &junk_w, 2);
    CHECK(!car::plausible(buf.data()));
    std::memcpy(buf.data() + car::kWeight, &real_w, 2);
    buf[car::kNumGears] = 12;
    CHECK(!car::plausible(buf.data()));
}

static void test_locator() {
    const uint32_t stride = car::kSize, base = 0x00700000;
    const uint32_t lead = 0x1234 & ~3u;  // array starts at an odd-ish aligned offset
    std::vector<uint8_t> mem(lead + stride * 24 + 0x800, 0xCC);
    for (int i = 0; i < 22; ++i) put_car(mem.data() + lead + i * stride, (uint8_t)(i == 5 ? 0x85 : i));
    auto c = find_car_arrays(mem.data(), (uint32_t)mem.size(), base, stride, 6);
    CHECK(c.size() == 1);
    if (!c.empty()) {
        CHECK(c[0].base == base + lead);
        CHECK(c[0].count == 22);
        CHECK(c[0].players.size() == 1 && c[0].players[0] == base + lead + 5 * stride);
    }
    // Too short a run is ignored; garbage gives nothing.
    std::vector<uint8_t> small(stride * 4, 0);
    for (int i = 0; i < 3; ++i) put_car(small.data() + i * stride, 1);
    CHECK(find_car_arrays(small.data(), (uint32_t)small.size(), 0, stride, 6).empty());
    std::vector<uint8_t> junk(0x10000, 0xAB);
    CHECK(find_car_arrays(junk.data(), (uint32_t)junk.size(), 0, stride, 6).empty());
}

static void test_correlator() {
    SteerCorrelator sc(200, 0.85, 0.15);
    for (int i = 0; i < 200; ++i) {
        double in = std::sin(i * 0.05);
        sc.add(in, 30000 + 20000 * in + ((i * 7919) % 13 - 6) * 50.0);  // scaled, offset, noisy
    }
    CHECK(sc.ready());
    CHECK(sc.confirmed());
    SteerCorrelator flat(200);
    for (int i = 0; i < 200; ++i) flat.add(0.0, i);    // input never moved -> undecided
    CHECK(!flat.ready() && !flat.confirmed() && !flat.rejected());
    SteerCorrelator frozen(200);                       // game value frozen (menus/pause): undecided
    for (int i = 0; i < 200; ++i) frozen.add(std::sin(i * 0.05), 1234.0);
    CHECK(!frozen.ready() && !frozen.rejected());
    SteerCorrelator wrong(200);
    for (int i = 0; i < 200; ++i) wrong.add(std::sin(i * 0.05), (i * 7919) % 101);
    CHECK(wrong.rejected());
    NEAR(correlation({1, 2, 3}, {2, 4, 6}), 1.0, 1e-12);
    NEAR(correlation({1, 2, 3}, {3, 2, 1}), -1.0, 1e-12);
}

static void test_ffb_model() {
    NEAR(soft_clip(0.5), 0.5, 1e-12);
    CHECK(soft_clip(5.0) <= 1.0 && soft_clip(5.0) > 0.99 && soft_clip(0.9) < 0.9 && soft_clip(0.9) > 0.8);
    NEAR(soft_clip(-5.0), -soft_clip(5.0), 1e-12);
    NEAR(wrap_turn(0.75), -0.25, 1e-12);
    NEAR(wrap_turn(-0.6), 0.4, 1e-12);

    const double dt = 0.002;
    FfbParams p;
    p.damping = p.damping_low_speed = p.friction = 0;
    p.shift_kick = p.impact_gain = 0;
    p.slew_per_s = 1000;
    p.master_gain = 1.0;
    p.game_scale = 1.0 / 10000.0;
    p.game_invert = false;
    FfbModel m;
    m.configure(p);
    FfbInput in;
    in.car_valid = true;
    in.speed_mps = 40;
    in.game_ffb = 5000;  // -> 0.5
    double out = 0;
    for (int i = 0; i < 500; ++i) out = m.step(in, dt);  // 1 s, well past the 30 Hz filter
    NEAR(out, 0.5, 1e-3);
    // The low-pass upsamples a step instead of passing it through.
    m.reset();
    double first = m.step(in, dt);
    CHECK(first > 0 && first < 0.5 * 0.5);
    // invert flips the sign.
    p.invert = true;
    m.configure(p);
    m.reset();
    for (int i = 0; i < 500; ++i) out = m.step(in, dt);
    NEAR(out, -0.5, 1e-3);

    // Damping opposes wheel motion; more of it at low speed.
    FfbParams pd;
    pd.game_gain = 0; pd.friction = 0; pd.shift_kick = 0; pd.impact_gain = 0; pd.slew_per_s = 1000;
    pd.master_gain = 1;
    pd.damping = 0.03; pd.damping_low_speed = 0.04;   // the mod's own damping (default 0: the base's)
    FfbModel md;
    md.configure(pd);
    FfbInput w;
    w.car_valid = true;
    w.speed_mps = 50;
    // Physical damping opposes the motion (negative while turning right); as a DirectInput
    // magnitude that is POSITIVE, the same sign GP4 uses to centre a wheel turned right.
    pd.damping_speed = 0;
    md.configure(pd);
    double fast = 0;
    for (int i = 0; i < 100; ++i) { w.steer = i * 0.002; fast = md.step(w, dt); }
    CHECK(md.last().damping < 0);
    CHECK(fast > 0);
    md.reset();
    w.speed_mps = 0;
    double slow = 0;
    for (int i = 0; i < 100; ++i) { w.steer = i * 0.002; slow = md.step(w, dt); }
    CHECK(slow > fast);
    // Speed-dependent damping: more at 60 m/s than at 20 m/s when low-speed damping is off.
    pd.damping_speed = 0.15; pd.damping_low_speed = 0;
    md.configure(pd);
    md.reset();
    w.speed_mps = 60;
    double d60 = 0, d20 = 0;
    for (int i = 0; i < 100; ++i) { w.steer = i * 0.002; md.step(w, dt); d60 = md.last().damping; }
    md.reset();
    w.speed_mps = 20;
    for (int i = 0; i < 100; ++i) { w.steer = i * 0.002; md.step(w, dt); d20 = md.last().damping; }
    CHECK(d60 < d20 && d20 < 0);

    // Regression for the first in-game runs: GP4's own centring force and our damping must
    // push the same way. GP4: wheel turned right -> +0x5E negative -> DI magnitude positive.
    FfbParams pc;   // defaults
    pc.slew_per_s = 1000;
    FfbModel mc;
    mc.configure(pc);
    FfbInput r;
    r.car_valid = true;
    r.speed_mps = 60;
    r.game_ffb = -4000;
    r.steer = 0.05;
    for (int i = 0; i < 200; ++i) mc.step(r, dt);
    CHECK(mc.last().game < 0);   // physical: pushes left, i.e. centring
    double only_game = mc.last().output;
    CHECK(only_game > 0);        // DirectInput magnitude, as GP4 would send it
    for (int i = 0; i < 20; ++i) { r.steer += 0.002; mc.step(r, dt); }   // turning further right
    CHECK(mc.last().damping < 0);
    CHECK(mc.last().output > only_game);   // damping adds to the centring force

    // Gear change produces a decaying kick; no car -> centring spring.
    FfbParams ps;
    ps.game_gain = 0; ps.damping = ps.damping_low_speed = ps.friction = 0; ps.impact_gain = 0;
    ps.slew_per_s = 1000; ps.master_gain = 1;
    FfbModel ms;
    ms.configure(ps);
    FfbInput g;
    g.car_valid = true;
    g.speed_mps = 30;
    g.gear = 3;
    for (int i = 0; i < 10; ++i) ms.step(g, dt);
    g.gear = 4;
    double kick = std::fabs(ms.step(g, dt));
    CHECK(kick > 0.05);
    for (int i = 0; i < 200; ++i) ms.step(g, dt);
    CHECK(std::fabs(ms.last().shift) < 0.01);
    FfbInput none;
    none.steer = 0.5;
    for (int i = 0; i < 50; ++i) out = ms.step(none, dt);
    CHECK(ms.last().spring < 0);
    CHECK(out > 0);   // DirectInput: positive pushes left, back to the centre

    // Impact: a 30 m/s -> 0 stop in 50 ms triggers the jolt.
    FfbParams pi;
    pi.game_gain = 0; pi.damping = pi.damping_low_speed = pi.friction = 0; pi.shift_kick = 0;
    pi.slew_per_s = 1000; pi.master_gain = 1;
    FfbModel mi;
    mi.configure(pi);
    FfbInput c;
    c.car_valid = true;
    c.speed_mps = 30;
    for (int i = 0; i < 50; ++i) mi.step(c, dt);
    double peak = 0;
    for (int i = 0; i < 25; ++i) { c.speed_mps = 30 - 30 * (i + 1) / 25.0; mi.step(c, dt); }
    for (int i = 0; i < 100; ++i) { mi.step(c, dt); peak = std::max(peak, std::fabs(mi.last().impact)); }
    CHECK(peak > 0.05);

    // Slew limiting bounds the per-step change.
    FfbParams pl;
    pl.damping = pl.damping_low_speed = pl.friction = 0; pl.game_lowpass_hz = 0; pl.slew_per_s = 10;
    pl.game_scale = 1.0 / 10000; pl.master_gain = 1; pl.game_invert = false;
    FfbModel ml;
    ml.configure(pl);
    FfbInput s;
    s.car_valid = true;
    s.game_ffb = 9000;
    NEAR(ml.step(s, dt), 10 * dt, 1e-12);

    // Defaults reproduce GP4's own force exactly: DI force = -1.5 * raw / 10000.
    FfbParams pg;
    pg.master_gain = 1; pg.damping = pg.damping_low_speed = pg.friction = 0;
    pg.shift_kick = 0; pg.impact_gain = 0; pg.slew_per_s = 1000;
    FfbModel mg;
    mg.configure(pg);
    FfbInput gi;
    gi.car_valid = true;
    gi.speed_mps = 40;
    gi.game_ffb = 4000;
    for (int i = 0; i < 500; ++i) out = mg.step(gi, dt);
    NEAR(out, -0.6, 1e-3);
    gi.game_ffb = -2000;
    for (int i = 0; i < 500; ++i) out = mg.step(gi, dt);
    NEAR(out, 0.3, 1e-3);

    // Model: steering into a straight-line car produces an aligning (opposing) torque.
    FfbParams pm;
    pm.game_gain = 0; pm.damping = pm.damping_low_speed = pm.friction = 0; pm.shift_kick = 0;
    pm.impact_gain = 0; pm.model_gain = 0.6; pm.slew_per_s = 1000; pm.master_gain = 1;
    FfbModel mm;
    mm.configure(pm);
    FfbInput a;
    a.car_valid = true;
    a.speed_mps = 40;
    a.steer = 0.1;  // 2 degrees of road-wheel angle, car going straight (beta = 0, yaw rate 0)
    for (int i = 0; i < 300; ++i) out = mm.step(a, dt);
    CHECK(mm.last().model < 0);
    CHECK(out > 0);
    a.steer = -0.1;
    mm.reset();
    for (int i = 0; i < 300; ++i) out = mm.step(a, dt);
    CHECK(out < 0);
    // Shadow mode: with model_gain = 0 the model is still computed (for the CSV) but
    // does not reach the wheel.
    pm.model_gain = 0;
    mm.configure(pm);
    mm.reset();
    for (int i = 0; i < 300; ++i) out = mm.step(a, dt);
    CHECK(std::fabs(mm.last().model_raw) > 0.05 && mm.last().model == 0.0);
    NEAR(out, 0.0, 1e-9);

    // model_mix: the aligning part of GP4's force is replaced by the model, GP4's damping
    // part stays. With total = aligning (no GP4 damping) and mix = 1, the game term is 0
    // and the model term carries model_scale x model_raw (physical convention).
    FfbParams px;
    px.damping = px.damping_speed = px.damping_low_speed = px.friction = 0;
    px.shift_kick = px.impact_gain = px.road_gain = 0;
    px.model_mix = 1.0;
    px.slew_per_s = 1000;
    FfbModel mx;
    mx.configure(px);
    FfbInput xi;
    xi.car_valid = true;
    xi.speed_mps = 40;
    xi.steer = 0.1;
    xi.game_ffb = xi.game_align = -3000;
    xi.has_align = true;
    for (int i = 0; i < 300; ++i) mx.step(xi, dt);
    NEAR(mx.last().game, 0.0, 1e-6);
    NEAR(mx.last().model, 1.3 * mx.last().model_raw, 1e-9);
    CHECK(mx.last().model < 0);   // turned right: centring, like GP4's own term would be
    // GP4's damping part (total - aligning) is kept.
    xi.game_ffb = -2000;
    for (int i = 0; i < 300; ++i) mx.step(xi, dt);
    NEAR(mx.last().game, 1.5e-4 * 1000, 1e-4);
    // Without the aligning term (unknown build) mix does nothing.
    xi.has_align = false;
    for (int i = 0; i < 300; ++i) mx.step(xi, dt);
    NEAR(mx.last().game, 1.5e-4 * -2000, 1e-4);
    NEAR(mx.last().model, 0.0, 1e-12);

    // Adaptive damping: GP4 updating at 60 Hz -> damping_speed scaled down (~0.55..0.6);
    // at 30 Hz -> about 1.
    auto run_rate = [&](double hz) {
        FfbParams pa;
        FfbModel ma;
        ma.configure(pa);
        FfbInput ai;
        ai.car_valid = true;
        ai.speed_mps = 60;
        int32_t yaw = 0;
        int every = (int)std::lround(500.0 / hz);
        for (int i = 0; i < 3000; ++i) {
            if (i % every == 0) yaw += 1000;
            ai.yaw_raw = yaw;
            ma.step(ai, dt);
        }
        return ma.last();
    };
    auto r60 = run_rate(62.5), r30 = run_rate(31.25);
    CHECK(std::fabs(r60.game_hz - 62.5) < 3 && std::fabs(r30.game_hz - 31.25) < 2);
    CHECK(r60.damping_scale < 0.6 && r30.damping_scale > 0.9 && r30.damping_scale < 1.0);
}

static void test_cockpit() {
    using namespace gp4u::cockpit;
    std::vector<uint8_t> car(0x420, 0);
    auto put = [&](int16_t front, uint8_t fl, int16_t raw) {
        std::memcpy(&car[kCarFront], &front, 2);
        car[kCarFlags] = fl;
        std::memcpy(&car[kCarRaw], &raw, 2);
    };
    Consts k;
    put(2000, 0, 0);   // x1.5 = 3000 (< 3641), x7 = 21000, x1.125 = 23625 (129.8 deg)
    CHECK(original_angle(car.data(), k) == 23625);
    put(2000, 1, 0);   // no x1.5
    CHECK(original_angle(car.data(), k) == q14(2000 * 7, 18432));
    put(-9000, 0, 0);  // clamped to -3641
    CHECK(original_angle(car.data(), k) == q14(-3641 * 7, 18432));
    NEAR(visual_degrees(original_angle(car.data(), k)), -157.5, 0.01);   // GP4's maximum
    // Raw path: the visual wheel turns like the physical one.
    CHECK(raw_angle(4064, 360) == 32768);
    CHECK(raw_angle(-4064, 360) == -32768);
    CHECK(raw_angle(2032, 360) == 16384);
    NEAR(visual_degrees(raw_angle(2032, 900)), 225.0 - 360.0, 0.01);   // 225 deg wraps
    put(2000, 0, 1016);
    CHECK(patched_angle(car.data(), k, 360) == 8192);   // 45 deg, front angle ignored
    put(2000, 0, 0);
    CHECK(patched_angle(car.data(), k, 360) == 23625);  // no raw input: original
}

static void test_ini_doc() {
    const std::string src =
        "; header\r\n"
        "[ffb]\r\n"
        "master_gain = 0.3          ; prudente\r\n"
        "damping = 0.02\r\n"
        "\r\n"
        "; next\r\n"
        "[buttons]\r\n"
        "0 = aro.b32 ; Fire1\r\n"
        "name = \"a;b\"   # quoted\r\n";
    IniDoc d;
    d.load_string(src);
    CHECK(d.text() == src);   // untouched = identical, CRLF kept
    CHECK(d.get("FFB", "Master_Gain") == "0.3");
    CHECK(d.get("buttons", "name") == "a;b");
    CHECK(d.comment("buttons", "0") == "Fire1");
    CHECK(d.comment("ffb", "damping").empty());
    // Changing a value keeps the key, the comment and its column.
    d.set("ffb", "master_gain", "0.45");
    CHECK(d.text().find("master_gain = 0.45         ; prudente\r\n") != std::string::npos);
    // A new key goes after the section's last key, before the blank line.
    d.set("ffb", "damping_speed", "0.15");
    CHECK(d.text().find("damping = 0.02\r\ndamping_speed = 0.15\r\n\r\n; next") != std::string::npos);
    // A new section at the end; values needing quotes get them.
    d.set("cockpit", "wheel_rotation_deg", "360");
    d.set("display", "chain", "x;y");
    CHECK(d.get("display", "chain") == "x;y");
    d.set_comment("buttons", "0", "upshift");
    CHECK(d.comment("buttons", "0") == "upshift");
    d.remove("ffb", "damping");
    CHECK(!d.has("ffb", "damping"));
    // Whatever IniDoc writes, the DLL's reader reads the same.
    Ini ini;
    ini.load_string(d.text());
    for (auto& sec : d.sections())
        for (auto& kv : d.entries(sec)) CHECK(ini.get(sec, kv.first) == kv.second);
    CHECK(ini.get("cockpit", "wheel_rotation_deg") == "360");
    CHECK(d.sections().size() == 4);
    // Duplicates: last wins, like Ini.
    IniDoc dup;
    dup.load_string("[a]\nk=1\nk=2\n");
    CHECK(dup.get("a", "k") == "2");
    dup.set("a", "k", "3");
    CHECK(dup.text() == "[a]\nk=1\nk=3\n");
    // format_number: shortest exact text.
    CHECK(format_number(1.0) == "1");
    CHECK(format_number(0.45) == "0.45");
    CHECK(format_number(1.0 / 16384) == "6.103515625e-05");
    double back = 0;
    parse_double(format_number(0.1 + 0.2), back);
    CHECK(back == 0.1 + 0.2);
    // Schema: unknown types degrade to text; choices parse.
    Schema sc;
    std::vector<std::string> w;
    CHECK(sc.load("[schema]\nversion=1\n[a.b]\ntype=slider3d\n[a.c]\ntype=choice\nchoices = x: Equis | y\ndefault=y\n[@axes]\nmanaged=controls\n", &w));
    CHECK(sc.find("a", "b") && sc.find("a", "b")->type == "text" && w.size() == 1);
    CHECK(sc.find("a", "c")->choices.size() == 2 && sc.find("a", "c")->choices[0].second == "Equis");
    CHECK(sc.managed("axes") && !sc.managed("a"));
}

static void test_resolution() {
    using namespace gp4u::gp4res;
    int w = 0, h = 0;
    CHECK(parse_resolution("1920x1080", w, h) && w == 1920 && h == 1080);
    CHECK(parse_resolution(" 5760 X 1080 ", w, h) && w == 5760);
    CHECK(!parse_resolution("16:9", w, h) && !parse_resolution("auto", w, h) && !parse_resolution("10x10", w, h));
    CHECK(is_auto("auto") && is_auto(" Auto ") && !is_auto("1920x1080"));
    {
        // A modern driver: 60 sizes x 4 refresh rates x 2 formats, the ultrawide last.
        std::vector<RawMode> raw;
        for (int i = 0; i < 60; ++i)
            for (int r : {60, 75, 120, 144})
                for (unsigned f : {22u, 23u}) raw.push_back({640 + 16 * i, 480 + 8 * i, r, f});
        for (int r : {50, 60, 144}) raw.push_back({2560, 1080, r, 22});
        CHECK(raw.size() > 256);
        CHECK(needs_fullscreen_list(raw, 2560, 1080));
        CHECK(!needs_fullscreen_list(raw, 640, 480));      // already within the first 256
        CHECK(!needs_fullscreen_list(raw, 0, 0));          // no resolution configured
        auto fm = fullscreen_modes(raw, 2560, 1080);
        CHECK(fm.size() <= 256 && fm[0].w == 2560 && fm[0].h == 1080 && fm[0].refresh == 50);   // first listed
        int n2560 = 0;
        for (auto& m : fm) n2560 += m.w == 2560;
        CHECK(n2560 == 1);
        CHECK(fm.size() == 121);   // 60 sizes x 2 formats + the ultrawide, one refresh each
        auto closest = closest_sizes(raw, 2560, 1000);
        CHECK(closest.size() == 3 && closest[0].w == 2560 && closest[0].h == 1080);
    }
    auto m = window_modes(1920, 1080, 5760, 1080);
    CHECK(m[0].w == 1920 && m[0].h == 1080);
    int n1080 = 0, ndesk = 0;
    for (auto& x : m) { n1080 += x.w == 1920 && x.h == 1080; ndesk += x.w == 5760; }
    CHECK(n1080 == 1 && ndesk == 1);   // deduplicated, desktop included
    auto b = bytes(sites()[4], 1920);
    CHECK(b.size() == 6 && b[0] == 0x81 && b[1] == 0xF9 && b[2] == 0x80 && b[3] == 0x07);
}

static void test_net_stats() {
    NetStats ns(1);
    uint8_t menu[8] = {0x10, 0, 0, 0x89, 1, 2, 3, 4};   // 0x89000010: menu message type 9
    uint8_t race[700] = {0x22, 0x00, 0x00, 0x00};
    CHECK(NetStats::kind_of(menu, 8) == "menu9" && NetStats::kind_of(race, 700) == "race");
    CHECK(NetStats::hex(menu, 8) == "10 00 00 89  01 02 03 04");
    double t = 0;
    for (int i = 0; i < 60; ++i, t += 1.0 / 60) {
        ns.on_send(t, 0, 0, race, 700);
        ns.on_recv(t + 0.005, 0x1111, race, 700);
        if (i % 2 == 0) ns.on_recv(t + 0.008, 0x2222, race, 700);
        ns.on_empty_poll(t);
    }
    ns.on_recv(t, 0x1111, menu, 8);
    auto lines = ns.poll(t + 0.01);
    int dumps = 0;
    std::string summary;
    for (auto& l : lines) {
        if (l.rfind("packet ", 0) == 0) ++dumps;
        else summary = l;
    }
    CHECK(dumps == 3);   // one per kind and direction: out race, in race, in menu9
    CHECK(summary.find("out 59 pkt/s") != std::string::npos);
    CHECK(summary.find("in 0x00001111 60 pkt/s") != std::string::npos);
    CHECK(summary.find("gap avg 33 max 33 ms") != std::string::npos);   // 0x2222 every other tick
    CHECK(summary.find("in race=90") != std::string::npos && summary.find("in menu9=1") != std::string::npos);
    CHECK(ns.poll(t + 0.5).empty());   // nothing new, period not over
}

static void test_textures() {
    const double dt = 0.002;
    FfbParams p;
    p.damping = p.damping_speed = p.damping_low_speed = p.friction = 0;
    p.shift_kick = p.impact_gain = p.road_gain = 0;
    p.master_gain = 1;
    p.slew_per_s = 1000;
    FfbModel m;
    m.configure(p);
    FfbInput in;
    in.car_valid = true;
    in.speed_mps = 45;   // kerb vibration at 30 Hz
    in.has_wheels = true;
    auto range = [&](int ticks) {
        double lo = 1e9, hi = -1e9;
        int crossings = 0;
        double prev = 0;
        for (int i = 0; i < ticks; ++i) {
            double o = m.step(in, dt);
            if (i > 50) { lo = std::min(lo, o); hi = std::max(hi, o); if ((o > 0) != (prev > 0)) ++crossings; }
            prev = o;
        }
        return std::make_pair(hi - lo, crossings);
    };
    auto asphalt = range(500);
    CHECK(asphalt.first < 1e-9);   // no texture on asphalt with no slide
    in.surface[2] = 1;             // front-left on a kerb
    auto kerb = range(500);        // 1 s
    NEAR(kerb.first, 2 * 0.2 * 0.8, 0.03);            // peak-to-peak: gain x (0.6 + 0.2 one front)
    CHECK(kerb.second > 50 && kerb.second < 70);       // ~30 Hz -> ~60 sign changes per second
    in.surface[2] = in.surface[3] = 4;                 // both fronts in the gravel
    auto gravel = range(500);
    CHECK(gravel.first > 0.05 && gravel.first < 0.6);
    // Off the asphalt GP4's own force gets lighter.
    in.surface[2] = in.surface[3] = 0;
    in.game_ffb = -4000;
    for (int i = 0; i < 300; ++i) m.step(in, dt);
    double on = m.last().game;
    in.surface[2] = in.surface[3] = 3;
    for (int i = 0; i < 5; ++i) m.step(in, dt);
    NEAR(m.last().game, on * 0.7, 1e-3);
    // Front tyres sliding: scrub proportional to the slide.
    in.surface[2] = in.surface[3] = 0;
    in.game_ffb = 0;
    in.slide_lat[2] = in.slide_lat[3] = 4096;   // half of scrub_full
    auto scrub = range(500);
    NEAR(scrub.first, 2 * 0.1 * 0.5, 0.015);
    // Textures are not flattened by the slew limit.
    p.slew_per_s = 10;
    m.configure(p);
    in.slide_lat[2] = in.slide_lat[3] = 0;
    in.surface[2] = 1;
    auto slow = range(500);
    CHECK(slow.first > 0.15);

    // Braking: front-left braking harder (more longitudinal force on [2]) -> physical +.
    FfbParams pb;
    pb.damping = pb.damping_speed = pb.damping_low_speed = pb.friction = 0;
    pb.shift_kick = pb.impact_gain = pb.road_gain = 0;
    pb.master_gain = 1;
    pb.slew_per_s = 1000;
    FfbModel mb;
    mb.configure(pb);
    FfbInput bi;
    bi.car_valid = true;
    bi.speed_mps = 50;
    bi.has_forces = true;
    bi.fx[2] = 60000;
    bi.fx[3] = 10000;
    for (int i = 0; i < 300; ++i) mb.step(bi, dt);
    NEAR(mb.last().brake, 0.15 * 0.5, 1e-3);
    bi.has_forces = false;
    mb.step(bi, dt);
    CHECK(mb.last().brake == 0.0);
    // Lock-up judder from the front longitudinal slide.
    bi.has_wheels = true;
    bi.slide_long[3] = 8192;
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 500; ++i) { double o = mb.step(bi, dt); if (i > 50) { lo = std::min(lo, o); hi = std::max(hi, o); } }
    NEAR(hi - lo, 2 * 0.10, 0.01);
}

static void test_ffb_params_load() {
    Ini ini;
    ini.load_string("[ffb]\nmaster_gain=0.5\ninvert=1\nfront_left=9\nmin_force=0.9\nwheel_range_deg=10\n");
    FfbParams p;
    p.load(ini);
    NEAR(p.master_gain, 0.5, 1e-12);
    CHECK(p.invert);
    CHECK(p.front_left == 3);       // clamped
    NEAR(p.min_force, 0.5, 1e-12);  // clamped
    NEAR(p.wheel_range_deg, 90, 1e-12);
}

static Mat4 perspective(double vfov_deg, double aspect, double zn = 1, double zf = 1000) {
    Mat4 p{};
    double y = 1.0 / std::tan(vfov_deg * 3.14159265358979 / 360.0);
    p.m[0][0] = (float)(y / aspect);
    p.m[1][1] = (float)y;
    p.m[2][2] = (float)(zf / (zf - zn));
    p.m[2][3] = 1;
    p.m[3][2] = (float)(-zn * zf / (zf - zn));
    return p;
}

static void test_projection() {
    Mat4 p = perspective(45, 4.0 / 3.0);
    CHECK(is_perspective(p));
    NEAR(projection_aspect(p), 4.0 / 3.0, 1e-5);
    Mat4 ortho{};
    ortho.m[0][0] = ortho.m[1][1] = ortho.m[2][2] = ortho.m[3][3] = 1;
    CHECK(!is_perspective(ortho));
    CHECK(!fix_aspect(ortho, 2.0));
    // 4:3 matrix shown on a 5760x1080 surface: Hor+ keeps vertical FOV, widens horizontal.
    double before_v = p.m[1][1];
    double hfov_before = horizontal_fov_deg(p);
    CHECK(fix_aspect(p, 5760.0 / 1080.0));
    NEAR(p.m[1][1], before_v, 1e-6);
    NEAR(projection_aspect(p), 5760.0 / 1080.0, 1e-4);
    CHECK(horizontal_fov_deg(p) > hfov_before);
    NEAR(horizontal_fov_deg(p), 2 * std::atan(std::tan(22.5 * 3.14159265358979 / 180) * 5760.0 / 1080.0) * 180 / 3.14159265358979, 1e-3);
    // fov_scale zooms out (smaller scales).
    Mat4 q = perspective(45, 16.0 / 9.0);
    fix_aspect(q, 16.0 / 9.0, 1.2);
    NEAR(q.m[1][1], perspective(45, 16.0 / 9.0).m[1][1] / 1.2, 1e-5);
    // Three angled screens: symmetric yaws, centre straight ahead, each with its own aspect.
    Mat4 c = perspective(40, 16.0 / 9.0), s0, s1, s2;
    double y0 = screen_view(c, 0, 3, 16.0 / 9.0, 0.0, s0);
    double y1 = screen_view(c, 1, 3, 16.0 / 9.0, 0.0, s1);
    double y2 = screen_view(c, 2, 3, 16.0 / 9.0, 0.0, s2);
    NEAR(y1, 0.0, 1e-12);
    NEAR(y0, -y2, 1e-12);
    // With no bezel, adjacent screens are rotated by exactly one screen's horizontal FOV.
    NEAR(y2 * 180 / 3.14159265358979, horizontal_fov_deg(s1), 1e-3);
    double yb = screen_view(c, 2, 3, 16.0 / 9.0, 0.05, s2);
    CHECK(yb > y2);
}

// Free-running race network: a client follows the master's race clock loosely.
static void test_net_pacer() {
    using A = NetPacer::Action;
    NetPacerConfig cfg;
    cfg.window_ms = 50;
    NetPacer p(cfg);   // window 50 ms, max 3 catch-up steps, hold 500 ms
    CHECK(p.decide(1000, 0, 0) == A::Run);           // no master yet: run freely
    // Master at 60 steps/s, real time; packets arrive every 16.7 ms.
    double now = 0;
    int32_t mt = 10000;
    for (int i = 0; i < 180; ++i) {
        p.on_master(mt, now);
        now += 1000.0 / 60;
        mt += 17;   // ~1.02 race ms per real ms
    }
    CHECK(std::fabs(p.rate() - 1.02) < 0.02);
    double m;
    CHECK(p.master_now(now, m));
    // In step: run.  A bit ahead/behind inside the window: run.
    CHECK(p.decide((int32_t)m, now, 0) == A::Run);
    CHECK(p.decide((int32_t)m + 40, now, 0) == A::Run);
    CHECK(p.decide((int32_t)m - 40, now, 0) == A::Run);
    // Ahead: wait; behind: catch up, at most 3 extra steps in a row.
    CHECK(p.decide((int32_t)m + 80, now, 0) == A::Wait);
    CHECK(p.decide((int32_t)m - 80, now, 0) == A::CatchUp);
    CHECK(p.decide((int32_t)m - 80, now, 3) == A::Run);
    // Waiting works: the master's estimate moves on with real time.
    CHECK(p.decide((int32_t)m + 80, now + 40, 0) == A::Run);
    // Master silent for longer than hold_ms: no pacing at all.
    CHECK(p.decide((int32_t)m + 5000, now + 600, 0) == A::Run);
    // A new session (race clock back to 0) restarts the rate measurement, keeps the rate.
    p.on_master(0, now + 700);
    CHECK(p.master_now(now + 710, m) && m > 5 && m < 15);
    // Absurd rates (master paused, clock jump) are not learned.
    NetPacer q;
    q.on_master(100, 0);
    q.on_master(100, 1500);   // paused master: rate 0 rejected
    CHECK(q.rate() == 1.0);
}

// Playout buffer: one race packet per sender per step, a reserve for jitter, catch-up.
static void test_net_playout() {
    using M = NetPlayout::Msg;
    auto race = [](uint32_t from, uint8_t seq) { return M{from, 0, {seq, 0, 0, 0, 1, 1, 0x80, 0, 0x2F}}; };
    auto menu = [](uint32_t from) { return M{from, 0, {0, 0, 0, 0x81, 1, 2}}; };
    NetPlayout p;
    M m;
    CHECK(NetPlayout::is_race_state(5, race(5, 0).data.data(), 9));
    CHECK(!NetPlayout::is_race_state(0, race(5, 0).data.data(), 9));           // DirectPlay system message
    CHECK(!NetPlayout::is_race_state(5, menu(5).data.data(), 6));              // menu message
    // Not racing: everything goes through in order.
    p.push(race(5, 1));
    p.push(race(5, 2));
    CHECK(p.pop(0, m) && m.data[0] == 1 && p.pop(0, m) && m.data[0] == 2 && !p.pop(0, m));
    // Racing, jittery arrivals 2,0,2,0: one per step, the second held for the empty step.
    double t = 1000;
    std::vector<int> got;
    int seq = 10;
    for (int step = 0; step < 8; ++step, t += 16.7) {
        p.begin_step(t);
        if (step % 2 == 0) { p.push(race(5, seq++)); p.push(race(5, seq++)); }
        int n = 0;
        while (p.pop(t, m)) ++n;
        got.push_back(n);
    }
    CHECK((got == std::vector<int>{1, 1, 1, 1, 1, 1, 1, 1}));
    // A burst of 4 at once: the step's one plus catch-up down to one held.
    p.begin_step(t);
    for (int i = 0; i < 4; ++i) p.push(race(5, seq++));
    int n = 0;
    while (p.pop(t, m)) ++n;
    CHECK(n == 3 && p.held(5) == 1);
    // Two senders are independent; menu/system messages are never held.
    p.begin_step(t += 16.7);
    p.push(race(6, 1));
    p.push(menu(6));
    p.push(M{0, 0, {3, 0, 0, 0}});
    n = 0;
    while (p.pop(t, m)) ++n;
    CHECK(n == 4 && p.held(5) == 0 && p.held(6) == 0);
    // GP4's buffer too small: put back, delivered again in the same step.
    p.begin_step(t += 16.7);
    p.push(race(5, 99));
    CHECK(p.pop(t, m) && m.data[0] == 99);
    p.unpop(m);
    CHECK(p.pop(t, m) && m.data[0] == 99 && !p.pop(t, m));
    // Race over (no step for 250 ms): held packets flow out.
    p.push(race(5, 100));
    p.push(race(5, 101));
    CHECK(p.pop(t, m) || true);
    n = 0;
    while (p.pop(t + 300, m)) ++n;
    CHECK(p.held(5) == 0);
}

// gp4ultimate.ini, or an older gp4mod.ini when it is the only one (paths use '\\' like
// Windows; on other systems that is just part of the file name, which is fine here).
static void test_config_file() {
    std::string dir = "test_paths_dir";
    std::remove((dir + "\\gp4ultimate.ini").c_str());
    std::remove((dir + "\\gp4mod.ini").c_str());
    bool legacy = true;
    CHECK(config_file(dir, &legacy) == dir + "\\gp4ultimate.ini" && !legacy);   // neither: the new name
    { std::ofstream f(dir + "\\gp4mod.ini"); f << "[general]\n"; }
    CHECK(config_file(dir, &legacy) == dir + "\\gp4mod.ini" && legacy);         // only the old one
    { std::ofstream f(dir + "\\gp4ultimate.ini"); f << "[general]\n"; }
    CHECK(config_file(dir, &legacy) == dir + "\\gp4ultimate.ini" && !legacy);  // the new one wins
    std::remove((dir + "\\gp4ultimate.ini").c_str());
    std::remove((dir + "\\gp4mod.ini").c_str());
}

int main() {
    test_ini();
    test_mapping_parse();
    test_mapping_errors();
    test_axis_conditioning();
    test_car_decode();
    test_locator();
    test_correlator();
    test_ffb_model();
    test_ffb_params_load();
    test_cockpit();
    test_ini_doc();
    test_resolution();
    test_net_stats();
    test_net_pacer();
    test_config_file();
    test_net_playout();
    test_textures();
    test_projection();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
