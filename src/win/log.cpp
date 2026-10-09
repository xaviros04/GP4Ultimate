#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace gp4u {

static FILE* g_log = nullptr;
static std::mutex g_log_mtx;
static LARGE_INTEGER g_t0, g_freq;

void log_open(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    if (g_log) return;
    g_log = std::fopen(path.c_str(), "w");
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
}

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    if (!g_log) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = (double)(now.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart;
    std::fprintf(g_log, "[%9.3f] ", t);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

std::string guid_str(const void* p) {
    const GUID& g = *(const GUID*)p;
    char buf[64];
    std::snprintf(buf, sizeof buf, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1,
                  g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
                  g.Data4[6], g.Data4[7]);
    return buf;
}

}  // namespace gp4u
