// QuattroStagioni - logger implementation.
#include "log.h"

#include <windows.h>

#include <cstdio>

namespace qs {
namespace log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_level = 1;

const char* tag(LogLevel lv) {
    switch (lv) {
        case LogLevel::Error: return "ERR ";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DBG ";
        case LogLevel::Trace: return "TRC ";
    }
    return "?   ";
}

}  // namespace

bool open(const std::wstring& path) {
    // Keep one previous log for comparison (only our own files are touched).
    std::wstring prev = path;
    size_t dot = prev.rfind(L'.');
    prev = (dot == std::wstring::npos ? prev : prev.substr(0, dot)) + L".previous.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_file != INVALID_HANDLE_VALUE;
}

void setLevel(int lv) { InterlockedExchange(&g_level, lv < 0 ? 0 : (lv > 3 ? 3 : lv)); }
int level() { return int(g_level); }
bool enabled(LogLevel lv) { return int(lv) <= int(g_level) && g_file != INVALID_HANDLE_VALUE; }

void vwrite(LogLevel lv, const char* fmt, va_list ap) {
    if (!enabled(lv)) return;
    DWORD lastError = GetLastError();  // logging must never disturb the game's error state
    char buf[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s ", t.wYear, t.wMonth, t.wDay,
                          t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), tag(lv));
    if (n < 0) n = 0;
    int m = std::vsnprintf(buf + n, sizeof(buf) - size_t(n) - 3, fmt, ap);
    if (m < 0) m = 0;
    size_t len = size_t(n) + size_t(m);
    if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;
    buf[len++] = '\r';
    buf[len++] = '\n';
    AcquireSRWLockExclusive(&g_lock);
    DWORD written = 0;
    if (g_file != INVALID_HANDLE_VALUE) WriteFile(g_file, buf, DWORD(len), &written, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
    SetLastError(lastError);
}

void write(LogLevel lv, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(lv, fmt, ap);
    va_end(ap);
}

void flush() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
    ReleaseSRWLockExclusive(&g_lock);
}

void close() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace log
}  // namespace qs
