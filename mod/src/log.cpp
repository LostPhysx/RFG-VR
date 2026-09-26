#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <string>

namespace rfgvr::log {

static FILE* g_file = nullptr;
static std::mutex g_mutex;

void init(const wchar_t* path) {
    std::scoped_lock lock{g_mutex};
    // _SH_DENYWR: readers (tail, cat) may open the log while the game holds it.
    if (!g_file) g_file = _wfsopen(path, L"w", _SH_DENYWR);
    if (!g_file) {
        // Locked by another (e.g. hung) instance: fall back to <name>.<pid>.log.
        std::wstring alt = path;
        size_t dot = alt.find_last_of(L'.');
        alt.insert(dot == std::wstring::npos ? alt.size() : dot, L"." + std::to_wstring(GetCurrentProcessId()));
        g_file = _wfsopen(alt.c_str(), L"w", _SH_DENYWR);
    }
}

void write(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::scoped_lock lock{g_mutex};
    if (g_file) {
        fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentThreadId(), msg);
        fflush(g_file);
    }
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

}  // namespace rfgvr::log
