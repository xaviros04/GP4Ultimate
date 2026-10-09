#pragma once
#include <string>

namespace gp4u {

void log_open(const std::string& path);
void logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string guid_str(const void* guid);   // "{XXXXXXXX-...}" from a GUID*

}  // namespace gp4u
