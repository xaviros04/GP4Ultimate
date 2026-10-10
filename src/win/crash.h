// Crash report for GP4 crashes: gp4ultimate_crash.txt (exception, registers, the code
// addresses on the stack as module+offset), the same text at the end of gp4ultimate.log,
// and gp4ultimate_crash.dmp (minidump), all in the game folder. Chains to the previous
// unhandled-exception filter, so Windows still reports the crash as before.
#pragma once
#include <string>

namespace gp4u {

void install_crash_handler(const std::string& dir);

}  // namespace gp4u
