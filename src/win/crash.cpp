// Crash report: when GP4 crashes, write what crashed and where to gp4ultimate_crash.txt
// (and the end of gp4ultimate.log), plus a minidump, so the cause can be found.
#include "crash.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace gp4u {

namespace {

std::string g_dir;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;
volatile LONG g_once = 0;

// Minimal dbghelp declarations (loaded at crash time; dbghelp.dll ships with Windows).
struct MiniDumpExceptionInfo {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};
typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, DWORD, MiniDumpExceptionInfo*, void*, void*);
constexpr DWORD kMiniDumpWithIndirectlyReferencedMemory = 0x40;

// "GP4.exe+0x35b61 (0x435b61)" for a code address.
void where(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        std::snprintf(out, n, "%s+0x%lx (0x%08lx)", base ? base + 1 : name, (unsigned long)(a - (uintptr_t)m),
                      (unsigned long)a);
    } else {
        std::snprintf(out, n, "0x%08lx", (unsigned long)a);
    }
}

bool is_code(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (a < 0x10000 || !VirtualQuery((void*)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE)
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void append(const std::string& path, const std::string& text) {
    HANDLE f = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(f);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_once, 1) == 0 && ep && ep->ExceptionRecord && ep->ContextRecord) {
        const EXCEPTION_RECORD& er = *ep->ExceptionRecord;
        const CONTEXT& c = *ep->ContextRecord;
        std::string r;
        char line[512], at[300];
        SYSTEMTIME st;
        GetLocalTime(&st);
        where((uintptr_t)er.ExceptionAddress, at, sizeof at);
        std::snprintf(line, sizeof line,
                      "\n==== GP4 crashed %04u-%02u-%02u %02u:%02u:%02u: exception 0x%08lx at %s, thread %lu\n",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (unsigned long)er.ExceptionCode,
                      at, (unsigned long)GetCurrentThreadId());
        r += line;
        if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
            std::snprintf(line, sizeof line, "  %s address 0x%08lx\n",
                          er.ExceptionInformation[0] == 8 ? "executing" : er.ExceptionInformation[0] ? "writing" : "reading",
                          (unsigned long)er.ExceptionInformation[1]);
            r += line;
        }
#if defined(_M_IX86) || defined(__i386__)
        std::snprintf(line, sizeof line,
                      "  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx eip %08lx\n",
                      (unsigned long)c.Eax, (unsigned long)c.Ebx, (unsigned long)c.Ecx, (unsigned long)c.Edx,
                      (unsigned long)c.Esi, (unsigned long)c.Edi, (unsigned long)c.Ebp, (unsigned long)c.Esp,
                      (unsigned long)c.Eip);
        r += line;
        // Return addresses on the stack (a rough call chain).
        r += "  code addresses on the stack:\n";
        const uintptr_t* sp = (const uintptr_t*)(uintptr_t)c.Esp;
        int shown = 0;
        for (int i = 0; i < 512 && shown < 24; ++i) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            uintptr_t v = sp[i];
            if (!is_code(v)) continue;
            where(v, at, sizeof at);
            std::snprintf(line, sizeof line, "    [esp+0x%03x] %s\n", i * 4, at);
            r += line;
            ++shown;
        }
#else
        (void)c;
#endif
        // Minidump next to the report.
        std::string dump = g_dir + "\\gp4ultimate_crash.dmp";
        if (HMODULE dbg = LoadLibraryA("dbghelp.dll")) {
            auto write = (MiniDumpWriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump");
            HANDLE f = CreateFileA(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (write && f != INVALID_HANDLE_VALUE) {
                MiniDumpExceptionInfo mei{GetCurrentThreadId(), ep, FALSE};
                if (write(GetCurrentProcess(), GetCurrentProcessId(), f, kMiniDumpWithIndirectlyReferencedMemory, &mei,
                          nullptr, nullptr))
                    r += "  minidump: gp4ultimate_crash.dmp\n";
            }
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        }
        r += "  Please send gp4ultimate_crash.txt, gp4ultimate_crash.dmp and gp4ultimate.log.\n";
        append(g_dir + "\\gp4ultimate_crash.txt", r);
        append(g_dir + "\\gp4ultimate.log", r);
    }
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_handler(const std::string& dir) {
    g_dir = dir;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(on_crash);
    if (prev != on_crash) g_prev = prev;
}

}  // namespace gp4u
