// QuattroStagioni - dedicated probe log.
#include "probe_log.h"

#include <windows.h>

#include <cstdio>

namespace qs::probe::plog {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
uint64_t g_written = 0, g_cap = 64ull << 20;
bool g_capped = false;
LARGE_INTEGER g_freq, g_t0;

}  // namespace

bool open(const std::wstring& path, uint32_t maxMB) {
    std::wstring prev = path;
    size_t dot = prev.rfind(L'.');
    prev = (dot == std::wstring::npos ? prev : prev.substr(0, dot)) + L".previous.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_cap = uint64_t(maxMB ? maxMB : 64) << 20;
    g_written = 0;
    g_capped = false;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    return g_file != INVALID_HANDLE_VALUE;
}

void line(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD lastError = GetLastError();
    char buf[4096];
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double s = double(t.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    int n = std::snprintf(buf, sizeof(buf), "[%9.3f] ", s);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(buf + n, sizeof(buf) - size_t(n) - 3, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    size_t len = size_t(n) + size_t(m);
    if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;
    buf[len++] = '\r';
    buf[len++] = '\n';
    AcquireSRWLockExclusive(&g_lock);
    DWORD w = 0;
    if (g_file != INVALID_HANDLE_VALUE && !g_capped) {
        if (g_written + len > g_cap) {
            const char msg[] = "probe log size cap reached (MaxLogMB): further output dropped\r\n";
            WriteFile(g_file, msg, DWORD(sizeof(msg) - 1), &w, nullptr);
            g_capped = true;
        } else {
            WriteFile(g_file, buf, DWORD(len), &w, nullptr);
            g_written += len;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    SetLastError(lastError);
}

void flush() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
    ReleaseSRWLockExclusive(&g_lock);
}

void close() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace qs::probe::plog
