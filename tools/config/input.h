// gp4ultimate-config: the real game controllers through Windows' own DirectInput (never the mod's
// proxy that sits in the same folder), polled live for the UI.
#pragma once
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gp4cfg {

struct Controller {
    std::string name;            // UTF-8
    std::string instance_name;   // UTF-8
    uint16_t vid = 0, pid = 0;
    std::string guid;            // {XXXXXXXX-...} upper-case
    int index = 0;               // position in DirectInput's enumeration (index: selector)
    bool ff = false;
    bool axis[8] = {};
    int buttons = 0, povs = 0;
    DIJOYSTATE2 st{};
    bool ok = false;             // last poll succeeded
    IDirectInputDevice8A* dev = nullptr;

    double axis01(int a) const;  // 0..1, 0.5 for absent axes
    bool button(int b) const { return b >= 0 && b < 128 && (st.rgbButtons[b] & 0x80); }
    long pov(int p) const { return p >= 0 && p < 4 ? (long)st.rgdwPOV[p] : -1; }
};

// Sign test: +30 % force, then -30 %, 1.5 s each, measuring where the wheel goes.
struct FfbTest {
    enum State { Idle, Positive, Negative, Done, Failed } state = Idle;
    int device = -1;
    double t0 = 0;
    long x0 = 0, x1 = 0, x2 = 0;
    int result = 0;        // +1 DirectInput standard (invert = 0), -1 opposite (invert = 1), 0 no movement
    std::string message;   // UTF-8, for the UI
    IDirectInputEffect* fx = nullptr;
};

class Input {
public:
    // dll: empty = %SystemRoot%\System32\dinput8.dll (tests pass a mock).
    bool init(HWND hwnd, const std::string& dll = "");
    void enumerate();
    void poll();
    std::vector<Controller>& controllers() { return list_; }
    const std::string& error() const { return error_; }

    void start_ffb_test(int device, double now);
    void update_ffb_test(double now);   // call every frame
    FfbTest& ffb_test() { return test_; }
    void shutdown();

private:
    void stop_test_effect();
    HWND hwnd_ = nullptr;
    IDirectInput8A* di_ = nullptr;
    std::vector<Controller> list_;
    std::string error_;
    FfbTest test_;
};

std::string to_utf8(const char* ansi);

}  // namespace gp4cfg
