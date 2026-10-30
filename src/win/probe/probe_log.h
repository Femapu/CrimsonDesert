// QuattroStagioni - dedicated probe log (bin64\scripts\QuattroStagioni\probe.log).
// Separate from QuattroStagioni.log because probes can be verbose; capped at MaxLogMB. Lines carry
// the seconds since probeStart so changes can be correlated with in-game actions.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>

namespace qs::probe::plog {

bool open(const std::wstring& path, uint32_t maxMB);  // keeps the previous run as probe.previous.log
void line(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(gnu_printf, 1, 2)))
#endif
    ;
void flush();
void close();

}  // namespace qs::probe::plog
