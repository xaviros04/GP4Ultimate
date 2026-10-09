// Composite-device mapping: which physical device/axis/button feeds each object of the
// virtual controller GP4 sees. Pure C++ (no Win32) so it is unit-tested natively.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ini.h"

namespace gp4u {

// Axes of a DIJOYSTATE2 position block, in DirectInput order.
enum AxisId : int { AX_X = 0, AX_Y, AX_Z, AX_RX, AX_RY, AX_RZ, AX_S0, AX_S1, AX_COUNT };
const char* axis_name(int a);
bool parse_axis_name(const std::string& s, int& out);

struct DeviceSelector {
    enum Kind { Name, VidPid, Guid, Index } kind = Name;
    std::string text;    // Name: case-insensitive substring; Guid: "{...}" upper-case
    uint16_t vid = 0, pid = 0;
    int index = 0;       // Index: n-th attached game controller (0-based)
};

struct NamedDevice {
    std::string alias;   // lower-case
    DeviceSelector sel;
};

// One physical axis with its conditioning. Input/output are normalised to 0..1.
struct AxisSource {
    std::string device;  // alias
    int axis = AX_X;
    bool invert = false;
    bool bipolar = false;   // centre-referenced (steering): deadzone/gamma act around 0.5
    double deadzone = 0.0;  // 0..1 (fraction of travel, or of half travel when bipolar)
    double gamma = 1.0;     // response curve exponent (>1 = softer near rest)
    double cal_min = 0.0;   // calibration: raw 0..1 values mapped to the 0..1 ends
    double cal_max = 1.0;
};

struct AxisBinding {
    int target = AX_X;
    enum Kind { Single, Combine } kind = Single;
    AxisSource a;  // Single: the source; Combine: accelerator (upper half)
    AxisSource b;  // Combine: brake (lower half)
};

struct ButtonBinding {
    int target = 0;      // virtual button 0..127
    std::string device;
    int button = 0;
};

struct PovBinding {
    int target = 0;      // virtual POV 0..3
    std::string device;
    int pov = 0;
};

struct MappingConfig {
    std::vector<NamedDevice> devices;
    std::vector<AxisBinding> axes;
    std::vector<ButtonBinding> buttons;      // explicit entries
    std::vector<std::string> auto_buttons;   // aliases whose buttons are appended in order
    std::vector<PovBinding> povs;
    std::string ffb_device;                  // alias that receives force feedback ("" = none)
    std::string virtual_name = "GP4 Composite Wheel";
    bool hide_physical = true;   // hide the devices used by the mapping
    bool hide_others = true;     // hide every other game controller too (GP4 opens all exclusively)
    bool virtual_first = true;

    bool enabled() const { return !devices.empty() && !axes.empty(); }
    const NamedDevice* device(const std::string& alias) const;
};

// Parses [devices] [virtual] [axes] [buttons] [povs]. Returns false with a message on
// the first error; warnings (ignored entries) are appended to `warnings`.
bool parse_mapping(const Ini& ini, MappingConfig& out, std::string& err,
                   std::vector<std::string>* warnings = nullptr);

bool parse_selector(const std::string& text, DeviceSelector& out, std::string& err);
// Does a selector pick this attached controller? The one rule the DLL and the
// configurator share. guid: "{XXXXXXXX-...}" upper-case; index: position in enumeration.
bool selector_matches(const DeviceSelector& sel, const std::string& product_name, const std::string& instance_name,
                      uint16_t vid, uint16_t pid, const std::string& guid, int index);
bool parse_axis_source(const std::string& text, AxisSource& out, std::string& err);

// Conditioning of one physical axis (raw01 in 0..1) -> 0..1.
double apply_source(const AxisSource& s, double raw01);
// Combined value of a binding given the raw 0..1 inputs of its source(s).
double apply_binding(const AxisBinding& b, double raw_a01, double raw_b01);

}  // namespace gp4u
