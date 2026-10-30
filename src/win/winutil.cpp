// QuattroStagioni - Win32 helpers.
#include "winutil.h"

namespace qs {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

std::wstring dirName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r.push_back(L'\\');
    for (wchar_t c : b) r.push_back(c == L'/' ? L'\\' : c);
    return r;
}

std::wstring modulePath(HMODULE m) {
    std::wstring buf(1024, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(m, &buf[0], DWORD(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return buf; }
        buf.resize(buf.size() * 2);
    }
}

bool fileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool fileSize(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

bool readFileRange(const std::wstring& path, uint64_t offset, size_t length, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz)) {
        uint64_t total = uint64_t(sz.QuadPart);
        if (length == 0) length = size_t(total > offset ? total - offset : 0);
        if (offset + length <= total) {
            out.resize(length);
            LARGE_INTEGER pos;
            pos.QuadPart = LONGLONG(offset);
            if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
                size_t done = 0;
                ok = true;
                while (done < length) {
                    DWORD chunk = DWORD(length - done > (1u << 26) ? (1u << 26) : length - done);
                    DWORD got = 0;
                    if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) { ok = false; break; }
                    done += got;
                }
            }
        }
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool readWholeFile(const std::wstring& path, std::vector<uint8_t>& out) { return readFileRange(path, 0, 0, out); }

std::wstring lowerW(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
    return s;
}

}  // namespace qs
