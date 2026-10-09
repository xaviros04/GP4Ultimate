// Owns the real game controllers (wheel, pedals, shifter/button box...) through our own
// IDirectInput8A instance and turns their state into the composite virtual state.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "core/mapping.h"
#include "di.h"

namespace gp4u {

struct VirtualState {
    double axis[AX_COUNT] = {};        // 0..1 after conditioning
    bool axis_bound[AX_COUNT] = {};
    uint8_t buttons[128] = {};
    DWORD pov[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
};

struct PhysDevice {
    std::string alias;
    std::string name;
    GUID instance{}, product{};
    IDirectInputDevice8A* dev = nullptr;
    DIDEVCAPS caps{};
    bool axis_present[AX_COUNT] = {};
    bool ffb = false;
    DIJOYSTATE2 state{};
    bool have_state = false;
    bool acquired = false;
};

class DeviceManager {
public:
    static DeviceManager& get();

    // Creates our own DirectInput instance via `create` and opens the mapped devices.
    // Safe to call repeatedly; only the first call does the work.
    bool init(DI8CreateFn create, const MappingConfig& cfg, bool ffb_background);
    bool active() const { return active_; }
    const MappingConfig& config() const { return cfg_; }

    bool is_claimed(const GUID& instance) const;   // a physical device used by the mapping
    int num_buttons() const { return num_buttons_; }
    int num_povs() const { return num_povs_; }

    void set_cooperative(HWND hwnd);               // from the game's SetCooperativeLevel
    HWND hwnd() const { return hwnd_; }
    void acquire();
    void unacquire();

    // Reads every device and returns the composite state.
    VirtualState poll();

    // --- force feedback (call with lock() held) -------------------------------------
    std::mutex& mutex() { return mtx_; }
    PhysDevice* ffb_device() { return ffb_; }
    int ffb_axis() const { return ffb_axis_; }      // DIJOYSTATE2 axis of the FFB wheel
    bool ffb_ready_locked();                       // exclusive + acquired, tries if not
    // Bumped every time the FFB wheel goes from not acquired to acquired. An exclusive
    // (re)acquire resets a force-feedback device and stops all its effects, so the engine
    // must re-create its effect whenever this changes (e.g. after a window focus change).
    uint32_t ffb_generation_locked() const { return ffb_gen_; }
    void mark_ffb_unacquired_locked() { if (ffb_) ffb_->acquired = false; }
    double steer_locked();                         // FFB wheel position -1..1 (raw + invert)
    DWORD virtual_to_real_axis_offset(int virtual_axis) const;  // for game effects

private:
    bool acquire_locked(PhysDevice& d);
    DeviceManager() = default;
    bool open(PhysDevice& d, const DIDEVICEINSTANCEA& inst);
    void read_locked(PhysDevice& d);
    void apply_coop_locked(PhysDevice& d);
    double raw01(const PhysDevice& d, int axis) const;
    PhysDevice* find(const std::string& alias);

    std::mutex mtx_;
    bool tried_ = false, active_ = false;
    MappingConfig cfg_;
    IDirectInput8A* di_ = nullptr;
    std::vector<PhysDevice> devs_;
    PhysDevice* ffb_ = nullptr;
    int ffb_axis_ = AX_X;
    bool ffb_axis_invert_ = false;
    HWND hwnd_ = nullptr;
    bool want_acquired_ = false;
    int num_buttons_ = 0, num_povs_ = 0;
    struct BtnSrc { int dev = -1, btn = 0; };
    BtnSrc btn_map_[128];
    struct PovSrc { int dev = -1, pov = 0; };
    PovSrc pov_map_[4];
    bool ffb_background_ = false;
    uint32_t ffb_gen_ = 0;
};

}  // namespace gp4u
