// Process-wide state of the mod DLL: settings loaded from gp4ultimate.ini next to the DLL.
#pragma once
#include <string>

#include "core/ini.h"
#include "core/mapping.h"
#include "di.h"

namespace gp4u {

enum class FfbMode { Off, Game, Telemetry, Mix };

struct ModSettings {
    std::string dir;              // folder of the DLL (the game folder)
    Ini ini;
    bool input_enabled = true;
    MappingConfig mapping;
    bool mapping_ok = false;
    FfbMode ffb_mode = FfbMode::Telemetry;
    double game_effects_gain = 1.0;
    bool ffb_background = false;
    std::string devtype = "driving";
    DI8CreateFn real_create = nullptr;   // system dinput8 (or the chained proxy)
};

ModSettings& mod();
const char* ffb_mode_name(FfbMode m);

}  // namespace gp4u
