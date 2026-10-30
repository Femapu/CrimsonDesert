// QuattroStagioni - thread-safe file logger (bin64\scripts\QuattroStagioni.log).
// Opened before any hook is installed and written with WriteFile only (never hooked), so logging
// from inside hooks cannot recurse.
#pragma once
#include <cstdarg>
#include <string>

namespace qs {

enum class LogLevel : int { Error = 0, Info = 1, Debug = 2, Trace = 3 };

namespace log {

// Opens the log, keeping the previous run as QuattroStagioni.previous.log.
bool open(const std::wstring& path);
void setLevel(int level);
int level();
bool enabled(LogLevel lv);
void write(LogLevel lv, const char* fmt, ...);
void vwrite(LogLevel lv, const char* fmt, va_list ap);
void flush();
void close();

}  // namespace log
}  // namespace qs

#define QS_ERR(...) ::qs::log::write(::qs::LogLevel::Error, __VA_ARGS__)
#define QS_INFO(...) ::qs::log::write(::qs::LogLevel::Info, __VA_ARGS__)
#define QS_DBG(...) do { if (::qs::log::enabled(::qs::LogLevel::Debug)) ::qs::log::write(::qs::LogLevel::Debug, __VA_ARGS__); } while (0)
#define QS_TRACE(...) do { if (::qs::log::enabled(::qs::LogLevel::Trace)) ::qs::log::write(::qs::LogLevel::Trace, __VA_ARGS__); } while (0)
