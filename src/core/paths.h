// Names of the files GP4 Ultimate reads and writes next to GP4.exe.
#pragma once
#include <fstream>
#include <string>

namespace gp4u {

constexpr const char* kIniName = "gp4ultimate.ini";
constexpr const char* kLegacyIniName = "gp4mod.ini";   // name before version 1.0

// The configuration file in `dir`: gp4ultimate.ini, or an older gp4mod.ini if that is the
// only one there (it keeps working; the configurator saves it as gp4ultimate.ini).
inline std::string config_file(const std::string& dir, bool* legacy = nullptr) {
    std::string cur = dir + "\\" + kIniName, old = dir + "\\" + kLegacyIniName;
    bool use_old = !std::ifstream(cur).good() && std::ifstream(old).good();
    if (legacy) *legacy = use_old;
    return use_old ? old : cur;
}

}  // namespace gp4u
