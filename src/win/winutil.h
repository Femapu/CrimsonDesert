// QuattroStagioni - small Win32 file/path helpers (used only during startup, before hooks exist).
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace qs {

std::string narrow(const std::wstring& w);   // UTF-16 -> UTF-8
std::wstring widen(const std::string& s);    // UTF-8 -> UTF-16
std::wstring dirName(const std::wstring& path);
std::wstring joinPath(const std::wstring& a, const std::wstring& b);
std::wstring modulePath(HMODULE m);
bool fileExists(const std::wstring& path);
bool fileSize(const std::wstring& path, uint64_t& size);
// Reads [offset, offset+length) (length 0 = whole file). Opens read-only with full sharing.
bool readFileRange(const std::wstring& path, uint64_t offset, size_t length, std::vector<uint8_t>& out);
bool readWholeFile(const std::wstring& path, std::vector<uint8_t>& out);
std::wstring lowerW(std::wstring s);

}  // namespace qs
