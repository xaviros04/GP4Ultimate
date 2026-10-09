// gp4u - dinput8.dll proxy for Grand Prix 4.
//
//   * composite controller: wheel + pedals + shifter/button box as one device (gp4ultimate.ini)
//   * telemetry force feedback: own FF engine fed from the player's car state
//
// Drop dinput8.dll + gp4ultimate.ini next to GP4.exe. Everything is configured in gp4ultimate.ini;
// gp4ultimate.log explains what was found and what the game asked for.
// Optional chaining: [general] chain = other_proxy.dll loads another dinput8 proxy (e.g.
// the repository's verification hook, renamed) and sits in front of it.
#include <mutex>

#include "cockpit_patch.h"
#include "net_hook.h"
#include "net_sync.h"
#include "physics_hook.h"
#include "core/mapping.h"
#include "device_manager.h"
#include "di_proxy.h"
#include "log.h"
#include "core/paths.h"
#include "core/version.h"
#include "mod.h"
#include "traits.h"

namespace gp4u {

ModSettings& mod() {
    static ModSettings s;
    return s;
}

const char* ffb_mode_name(FfbMode m) {
    switch (m) {
        case FfbMode::Off: return "off";
        case FfbMode::Game: return "game";
        case FfbMode::Telemetry: return "telemetry";
        case FfbMode::Mix: return "mix";
    }
    return "?";
}

DWORD virtual_devtype() {
    const std::string& t = mod().devtype;
    if (t == "joystick") return DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8) | DIDEVTYPE_HID;
    if (t == "gamepad") return DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
    return DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_DUALPEDALS << 8) | DIDEVTYPE_HID;
}

static HMODULE g_self = nullptr;
static std::once_flag g_init_once;

static void load_settings() {
    ModSettings& s = mod();
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    s.dir = path;
    size_t slash = s.dir.find_last_of("\\/");
    s.dir = slash == std::string::npos ? "." : s.dir.substr(0, slash);
    bool legacy = false;
    s.ini.load_file(config_file(s.dir, &legacy));
    if (s.ini.get_bool("general", "log", true)) log_open(s.dir + "\\gp4ultimate.log");
    if (legacy) logf("config: reading the old gp4mod.ini (open gp4ultimate-config.exe and save to rename it)");
    logf("GP4 Ultimate %s (dinput8.dll) loaded from %s", kVersion, s.dir.c_str());

    s.input_enabled = s.ini.get_bool("general", "input", true);
    std::string err;
    std::vector<std::string> warnings;
    s.mapping_ok = parse_mapping(s.ini, s.mapping, err, &warnings);
    if (!s.mapping_ok) logf("config error: %s - input stays pass-through", err.c_str());
    for (auto& w : warnings) logf("config warning: %s", w.c_str());

    std::string m = Ini::lower(s.ini.get("ffb", "mode", "telemetry"));
    s.ffb_mode = m == "off" ? FfbMode::Off : m == "game" ? FfbMode::Game : m == "mix" ? FfbMode::Mix : FfbMode::Telemetry;
    s.game_effects_gain = s.ini.get_double("ffb", "game_effects_gain", 1.0);
    s.ffb_background = s.ini.get_bool("ffb", "background", false);
    s.devtype = Ini::lower(s.ini.get("virtual", "devtype", "driving"));
    logf("settings: input=%d mapping=%s ffb=%s devtype=%s", s.input_enabled ? 1 : 0,
         s.mapping_ok ? (s.mapping.enabled() ? "ok" : "empty") : "invalid", ffb_mode_name(s.ffb_mode), s.devtype.c_str());

    // The real DirectInput: a chained proxy if configured, else the system DLL.
    std::string chain = s.ini.get("general", "chain", "");
    HMODULE real = nullptr;
    if (!chain.empty()) {
        std::string p = (chain.find(':') == std::string::npos) ? s.dir + "\\" + chain : chain;
        real = LoadLibraryA(p.c_str());
        logf("chain: %s %s", p.c_str(), real ? "loaded" : "NOT FOUND - using system dinput8");
    }
    if (!real) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);   // SysWOW64 for this 32-bit process
        lstrcatA(sys, "\\dinput8.dll");
        real = LoadLibraryA(sys);
    }
    s.real_create = real ? (DI8CreateFn)GetProcAddress(real, "DirectInput8Create") : nullptr;
    if (!s.real_create) logf("FATAL: no real DirectInput8Create");

    // Per-wheel forces of the player's car for the FFB (braking feel): hook at the end of
    // GP4's per-car physics step (only on the known build, checked byte for byte).
    if (s.ini.get_bool("telemetry", "physics_hook", true)) {
        std::string why;
        if (install_physics_hook(gp4_phys_targets(), &why)) logf("telemetry: physics-step hook on (per-wheel forces)");
        else logf("telemetry: physics-step hook not installed: %s", why.c_str());
    }

    // Network log (diagnostics for the online work): DirectPlay calls and traffic.
    bool net_log = s.ini.get_bool("net", "log", true), freerun = s.ini.get_bool("net", "freerun", true);
    bool playout = freerun && s.ini.get_bool("net", "playout", true);
    if (net_log || playout) install_net_hooks((int)s.ini.get_int("net", "dump_packets", 3), net_log, playout);

    // Free-running online races: no waiting for the other machines every step.
    if (freerun) {
        NetPacerConfig pc;
        pc.window_ms = s.ini.get_double("net", "sync_window_ms", pc.window_ms);
        pc.max_wait_ms = s.ini.get_double("net", "max_wait_ms", pc.max_wait_ms);
        pc.max_catchup = (int)s.ini.get_int("net", "max_catchup", pc.max_catchup);
        std::string why;
        net_sync_set_step_callback(playout ? net_playout_begin_step : nullptr);
        if (install_net_sync(gp4_net_sync_targets(), pc, &why))
            logf("net: free-running races on (clock window %.0f ms, wait <= %.0f ms, catch-up <= %d steps)",
                 pc.window_ms, pc.max_wait_ms, pc.max_catchup);
        else
            logf("net: free-running races not installed: %s", why.c_str());
    }

    // Cockpit steering wheel turning like the physical one (GP4.exe 0x409a47).
    double rot = s.ini.get_double("cockpit", "wheel_rotation_deg", 360.0);
    if (rot > 0) {
        std::string why;
        if (install_cockpit_wheel(rot, gp4_cockpit_targets(), &why))
            logf("cockpit: steering wheel turns %.0f degrees lock to lock (raw input; AI/keyboard cars as original)", rot);
        else
            logf("cockpit: wheel_rotation_deg not applied: %s", why.c_str());
    }
}

bool ensure_device_manager() {
    ModSettings& s = mod();
    if (!s.input_enabled || !s.mapping_ok || !s.mapping.enabled() || !s.real_create) return false;
    return DeviceManager::get().init(s.real_create, s.mapping, s.ffb_background);
}

}  // namespace gp4u

using namespace gp4u;

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out,
                                             LPUNKNOWN outer) {
    std::call_once(g_init_once, load_settings);
    ModSettings& s = mod();
    if (!s.real_create) return DIERR_GENERIC;
    HRESULT hr = s.real_create(hinst, version, riid, out, outer);
    if (FAILED(hr) || !out || !*out || !s.input_enabled) return hr;
    if (riid == IID_IDirectInput8A) {
        *out = static_cast<IDirectInput8A*>(new ProxyDirectInput<TraitsA>((IDirectInput8A*)*out));
        logf("game: DirectInput8Create(A, version 0x%04lx) -> proxied", (unsigned long)version);
    } else if (riid == IID_IDirectInput8W) {
        *out = static_cast<IDirectInput8W*>(new ProxyDirectInput<TraitsW>((IDirectInput8W*)*out));
        logf("game: DirectInput8Create(W, version 0x%04lx) -> proxied", (unsigned long)version);
    } else {
        logf("game: DirectInput8Create(%s) not proxied", guid_str(&riid).c_str());
    }
    return hr;
}

// The COM-server exports of the system dinput8.dll, forwarded in case GP4 imports them
// (a missing import would stop the game from starting at all).
template <typename F>
static F real_export(const char* name) {
    std::call_once(g_init_once, load_settings);
    static HMODULE sys = nullptr;
    if (!sys) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        lstrcatA(p, "\\dinput8.dll");
        sys = LoadLibraryA(p);
    }
    return sys ? (F)GetProcAddress(sys, name) : nullptr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    auto f = real_export<HRESULT(WINAPI*)()>("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID i, LPVOID* o) {
    auto f = real_export<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject");
    return f ? f(c, i, o) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer() {
    auto f = real_export<HRESULT(WINAPI*)()>("DllRegisterServer");
    return f ? f() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    auto f = real_export<HRESULT(WINAPI*)()>("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
