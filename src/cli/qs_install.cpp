// QuattroStagioni - qs_install, installer / uninstaller / pre-flight check for end users.
//
//   qs_install                      install (asks for confirmation; finds the game through Steam)
//   qs_install --check              dry run: readable report, writes nothing
//   qs_install --uninstall          restores bin64 exactly as it was before the first install
//   options: --game "<Crimson Desert dir>"  --from "<release dir>"  --steam "<Steam dir>"  --yes
//            --purge (with --uninstall: also delete the generated season packs)
//            --reset-config (overwrite an edited QuattroStagioni.ini, a backup is kept)
//            --install-loader "<winmm.dll of Ultimate ASI Loader>"  --no-archives  --report <file>
//            --test-build <size>:<timestamp> (tests only: accept a fake executable)
//
// Safety rules (requirement 6, CLAUDE.md):
//   * game files are never modified or deleted: every write or delete goes through allowedTarget(),
//     which only accepts paths under bin64\scripts\, the loader ini (bin64\winmm.ini) and, with
//     --install-loader, the loader DLL itself;
//   * everything replaced is backed up first under bin64\scripts\QuattroStagioni\install\backup\ and
//     every copy is verified with SHA-256 (destination re-read from disk);
//   * the uninstall manifest (bin64\scripts\QuattroStagioni\install\uninstall.txt) lists every file
//     created or replaced, every directory created and the QuattroStagioni files that already existed,
//     so --uninstall can put everything back.
// Console output is Italian (the players' language); comments are English like the rest of the code.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <shellapi.h>
#define QS_ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define QS_ISATTY(f) isatty(fileno(f))
#endif

#include "ini.h"
#include "known_builds.h"
#include "lookup3.h"
#include "pack/qsp_io.h"
#include "pamt.h"

namespace fs = std::filesystem;

namespace {

const char* const kToolVersion = "qs_install 0.3.0";
const char* const kManifestHeader = "# QuattroStagioni uninstall manifest v1 (scritto da qs_install, non modificare)";

// ------------------------------------------------------------------ strings and paths
fs::path P(const std::string& u8) { return fs::u8path(u8); }
std::string S(const fs::path& p) {
    fs::path q = p;
    q.make_preferred();
    return q.u8string();
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
bool endsWithI(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && iequals(s.substr(s.size() - suf.size()), suf);
}
std::string fmt(const char* f, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}
std::string relKey(const fs::path& rel) {  // manifest form: forward slashes
    std::string s = rel.generic_u8string();
    return s;
}
std::string nowText() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// ------------------------------------------------------------------ SHA-256 (FIPS 180-4)
class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof h_);
        len_ = 0;
        used_ = 0;
    }
    void update(const uint8_t* p, size_t n) {
        len_ += n;
        while (n > 0) {
            size_t take = std::min(n, size_t(64) - used_);
            std::memcpy(buf_ + used_, p, take);
            used_ += take;
            p += take;
            n -= take;
            if (used_ == 64) {
                block(buf_);
                used_ = 0;
            }
        }
    }
    std::string hex() {
        uint64_t bits = len_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used_ != 56) update(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (56 - 8 * i));
        update(l, 8);
        static const char* hx = "0123456789abcdef";
        std::string s;
        for (uint32_t v : h_)
            for (int i = 28; i >= 0; i -= 4) s += hx[(v >> i) & 15];
        return s;
    }

private:
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* b) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = bb;
            bb = a;
            a = t1 + t2;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t len_;
    size_t used_;
};

std::string sha256Bytes(const std::string& data) {
    Sha256 s;
    s.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return s.hex();
}

bool sha256File(const fs::path& p, std::string& hex) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    Sha256 s;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        std::streamsize n = f.gcount();
        if (n > 0) s.update(reinterpret_cast<const uint8_t*>(buf.data()), size_t(n));
    }
    if (f.bad()) return false;
    hex = s.hex();
    return true;
}

bool readFile(const fs::path& p, std::string& out, uint64_t maxBytes = UINT64_MAX) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.clear();
    std::vector<char> buf(1 << 16);
    while (f && out.size() < maxBytes) {
        f.read(buf.data(), std::streamsize(buf.size()));
        std::streamsize n = f.gcount();
        if (n > 0) out.append(buf.data(), size_t(n));
    }
    if (out.size() > maxBytes) out.resize(size_t(maxBytes));
    return !f.bad();
}

bool pathExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}
bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
uint64_t sizeOf(const fs::path& p) {
    std::error_code ec;
    uint64_t s = fs::file_size(p, ec);
    return ec ? 0 : s;
}
std::vector<fs::path> listDir(const fs::path& d) {  // sorted, empty on error
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}
// Finds a child of `dir` by name, case-insensitive (Windows semantics on any host).
fs::path childI(const fs::path& dir, const std::string& name) {
    fs::path direct = dir / P(name);
    if (pathExists(direct)) return direct;
    for (const fs::path& c : listDir(dir))
        if (iequals(c.filename().u8string(), name)) return c;
    return direct;
}

// ------------------------------------------------------------------ console and log
struct Console {
    std::string log;  // everything printed, for the install log and --report
    void print(const std::string& s) {
        std::fputs(s.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        log += s;
        log += "\r\n";
    }
} con;

void say(const char* f, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    con.print(buf);
}

enum class Lv { Ok, Info, Warn, Err };
struct Report {
    int warnings = 0, errors = 0;
    void add(Lv lv, const std::string& text, const std::string& hint = "") {
        const char* tag = lv == Lv::Ok ? "[OK]     " : lv == Lv::Info ? "[INFO]   " : lv == Lv::Warn ? "[AVVISO] " : "[ERRORE] ";
        if (lv == Lv::Warn) ++warnings;
        if (lv == Lv::Err) ++errors;
        con.print(std::string("  ") + tag + text);
        if (!hint.empty()) {
            size_t start = 0;
            while (start <= hint.size()) {
                size_t nl = hint.find('\n', start);
                std::string line = hint.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                con.print("             " + line);
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
    }
};

void section(const std::string& title) {
    con.print("");
    con.print("== " + title);
}

// ------------------------------------------------------------------ options
struct Options {
    enum Mode { Install, Check, Uninstall, Help, Version, PrintGame } mode = Install;
    std::string game, from, steam, installLoader, report;
    bool yes = false, purge = false, resetConfig = false, noArchives = false, noPause = false;
    std::vector<qs::builds::KnownExe> testBuilds;
};
Options opt;

// ------------------------------------------------------------------ safety guard
// The only places qs_install may write or delete: bin64\scripts\**, the loader ini, and with
// --install-loader the loader DLL. Anything else throws (the operation is never attempted).
struct Guard {
    fs::path bin64;
    std::string loaderIni = "winmm.ini", loaderDll;  // loaderDll empty unless --install-loader
    void check(const fs::path& target, const char* what) const {
        fs::path rel = target.lexically_normal().lexically_relative(bin64.lexically_normal());
        bool ok = false;
        if (!rel.empty() && rel.begin()->u8string() != ".." && !rel.is_absolute()) {
            std::vector<std::string> parts;
            for (const fs::path& c : rel) parts.push_back(c.u8string());
            bool dotdot = false;
            for (const std::string& c : parts) dotdot |= c == "..";
            if (!dotdot) {
                if (parts.size() >= 2 && iequals(parts[0], "scripts")) ok = true;
                else if (parts.size() == 1 && iequals(parts[0], "scripts")) ok = std::string(what) == "mkdir" || std::string(what) == "rmdir";
                else if (parts.size() == 1 && iequals(parts[0], loaderIni)) ok = true;
                else if (parts.size() == 1 && !loaderDll.empty() && iequals(parts[0], loaderDll)) ok = true;
            }
        }
        if (!ok) throw std::runtime_error("operazione bloccata dalla protezione dei file del gioco (" + std::string(what) + "): " + S(target));
    }
} guard;

bool safeMkdirs(const fs::path& d, std::vector<fs::path>* created) {
    if (isDir(d)) return true;
    if (!d.has_parent_path() || d.parent_path() == d) return false;
    if (!isDir(d.parent_path()) && !safeMkdirs(d.parent_path(), created)) return false;
    guard.check(d, "mkdir");
    std::error_code ec;
    if (!fs::create_directory(d, ec) && !isDir(d)) return false;
    if (created) created->push_back(d);
    return true;
}

bool safeRemoveFile(const fs::path& p) {
    guard.check(p, "delete");
    std::error_code ec;
    fs::remove(p, ec);
    return !ec && !pathExists(p);
}

bool safeRemoveEmptyDir(const fs::path& d) {
    guard.check(d, "rmdir");
    if (!listDir(d).empty()) return false;
    std::error_code ec;
    return fs::remove(d, ec) && !ec;
}

// Writes `data` to `dst` (truncating) and verifies it by re-reading the file.
// Plain stream I/O on purpose: std::filesystem::copy_file(overwrite_existing) and rename-over are
// unreliable with some MinGW runtimes and under wine.
bool writeAndVerify(const fs::path& dst, const std::string& data, const std::string& wantSha, std::string& err) {
    {
        std::ofstream f(dst, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "impossibile scrivere " + S(dst) + " (file in uso o cartella protetta?)";
            return false;
        }
        f.write(data.data(), std::streamsize(data.size()));
        f.flush();
        if (!f) {
            err = "scrittura non riuscita (disco pieno?)";
            return false;
        }
    }
    std::string got;
    if (!sha256File(dst, got)) {
        err = "impossibile rileggere " + S(dst);
        return false;
    }
    if (got != wantSha) {
        err = "SHA-256 diverso dopo la copia (atteso " + wantSha.substr(0, 16) + "..., letto " + got.substr(0, 16) + "...)";
        return false;
    }
    return true;
}

// Copies and verifies: the destination is re-read and must hash to `wantSha`.
bool safeCopyVerified(const fs::path& src, const fs::path& dst, const std::string& wantSha, std::string& err) {
    guard.check(dst, "write");
    std::string data;
    if (!readFile(src, data)) {
        err = "impossibile leggere " + S(src);
        return false;
    }
    if (sha256Bytes(data) != wantSha) {
        err = "il file di origine " + S(src) + " non corrisponde allo SHA-256 atteso";
        return false;
    }
    return writeAndVerify(dst, data, wantSha, err);
}

bool safeWriteVerified(const fs::path& dst, const std::string& data, std::string& err) {
    guard.check(dst, "write");
    return writeAndVerify(dst, data, sha256Bytes(data), err);
}

// ------------------------------------------------------------------ Steam and game folder
bool isGameRoot(const fs::path& r) { return isFile(childI(childI(r, "bin64"), "CrimsonDesert.exe")); }

// Quoted tokens of a Valve KeyValues (vdf/acf) file, with \\ and \" unescaped.
std::vector<std::string> vdfTokens(const std::string& t) {
    std::vector<std::string> out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] != '"') continue;
        std::string s;
        for (++i; i < t.size() && t[i] != '"'; ++i) {
            if (t[i] == '\\' && i + 1 < t.size()) {
                ++i;
                s += t[i] == 'n' ? '\n' : t[i] == 't' ? '\t' : t[i];
            } else {
                s += t[i];
            }
        }
        out.push_back(s);
    }
    return out;
}

std::string vdfValue(const std::string& text, const std::string& key) {
    std::vector<std::string> tok = vdfTokens(text);
    for (size_t i = 0; i + 1 < tok.size(); ++i)
        if (iequals(tok[i], key)) return tok[i + 1];
    return "";
}

std::vector<fs::path> steamRoots() {
    std::vector<fs::path> roots;
    if (!opt.steam.empty()) roots.push_back(P(opt.steam));
#ifdef _WIN32
    struct Key {
        HKEY root;
        const wchar_t* sub;
        const wchar_t* value;
    } keys[] = {{HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
                {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
                {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"}};
    for (const Key& k : keys) {
        wchar_t buf[1024];
        DWORD size = sizeof buf;
        if (RegGetValueW(k.root, k.sub, k.value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            roots.push_back(fs::path(std::wstring(buf)));
    }
    roots.push_back(P("C:\\Program Files (x86)\\Steam"));
#else
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(P(std::string(home) + "/.steam/steam"));
        roots.push_back(P(std::string(home) + "/.local/share/Steam"));
    }
#endif
    return roots;
}

std::vector<fs::path> steamLibraries(const fs::path& steam) {
    std::vector<fs::path> libs{steam};
    for (const char* rel : {"steamapps/libraryfolders.vdf", "config/libraryfolders.vdf"}) {
        std::string text;
        if (!readFile(steam / P(rel), text)) continue;
        std::vector<std::string> tok = vdfTokens(text);
        for (size_t i = 0; i + 1 < tok.size(); ++i)
            if (iequals(tok[i], "path")) libs.push_back(P(tok[i + 1]));
    }
    return libs;
}

void addUnique(std::vector<fs::path>& v, const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec) c = p;
    for (const fs::path& x : v)
        if (iequals(S(x), S(c))) return;
    v.push_back(c);
}

std::vector<fs::path> findGameFolders(const fs::path& exeDir) {
    std::vector<fs::path> found;
    // 1. the release was unpacked inside the game folder (or launched from it)
    for (fs::path base : {exeDir, fs::current_path()}) {
        for (fs::path p = base; !p.empty(); p = p.parent_path()) {
            if (isGameRoot(p)) addUnique(found, p);
            if (p == p.parent_path()) break;
        }
    }
    // 2. Steam libraries
    for (const fs::path& steam : steamRoots()) {
        if (!isDir(steam)) continue;
        for (const fs::path& lib : steamLibraries(steam)) {
            fs::path apps = childI(lib, "steamapps");
            std::string acf;
            std::string installdir;
            if (readFile(apps / P(std::string("appmanifest_") + qs::builds::kSteamAppId + ".acf"), acf))
                installdir = vdfValue(acf, "installdir");
            for (const std::string& name : {installdir, std::string("Crimson Desert"), std::string("CrimsonDesert")}) {
                if (name.empty()) continue;
                fs::path g = childI(childI(apps, "common"), name);
                if (isGameRoot(g)) addUnique(found, g);
            }
        }
    }
    return found;
}

// ------------------------------------------------------------------ game checks
struct ExeInfo {
    bool readable = false;
    uint64_t size = 0;
    uint32_t ts = 0;
    std::string label;  // empty = unknown build
};

ExeInfo readExe(const fs::path& exe) {
    ExeInfo e;
    std::string h;
    if (!readFile(exe, h, 4096) || h.size() < 0x40 || h[0] != 'M' || h[1] != 'Z') return e;
    uint32_t pe;
    std::memcpy(&pe, h.data() + 0x3C, 4);
    if (pe + 12 > h.size() || std::memcmp(h.data() + pe, "PE\0\0", 4) != 0) return e;
    std::memcpy(&e.ts, h.data() + pe + 8, 4);
    e.size = sizeOf(exe);
    e.readable = true;
    if (const qs::builds::KnownExe* k = qs::builds::findKnownExe(e.size, e.ts)) e.label = k->label;
    for (const auto& t : opt.testBuilds)
        if (t.size == e.size && t.timestamp == e.ts) e.label = t.label;
    return e;
}

std::string hexOf(const std::string& b) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) {
        s += hx[c >> 4];
        s += hx[c & 15];
    }
    return s;
}

struct DllInfo {
    std::string name, kind;
    bool asiLoader = false;
};

DllInfo classifyDll(const fs::path& p) {
    DllInfo d;
    d.name = p.filename().u8string();
    std::string b;
    readFile(p, b, 64ull << 20);
    auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
    if (has("IsUltimateASILoader")) {
        d.kind = "Ultimate ASI Loader";
        d.asiLoader = true;
    } else if (has("ReShadeRegisterAddon") || has("crosire")) {
        d.kind = "ReShade";
    } else if (has("OptiScaler")) {
        d.kind = "OptiScaler";
    } else if (has("SpecialK") || has("Special K")) {
        d.kind = "Special K";
    } else if (has("dxvk") || has("DXVK")) {
        d.kind = "DXVK";
    } else {
        d.kind = "sconosciuto";
    }
    return d;
}

struct GroupScan {
    std::string name;  // "0012" or "dlc/dlc_01/0015"
    uint32_t selfHash = 0;
    std::map<uint32_t, uint64_t> pazSizes;  // actual size on disk
};

struct State {
    fs::path exeDir, release, game, bin64, scripts, qsDir, installDir, manifestPath;
    std::string releaseVersion = "sviluppo";
    ExeInfo exe;
    bool paverOk = false;
    std::vector<DllInfo> proxies;
    std::string loaderName;  // dll name of the single Ultimate ASI Loader, empty if none
    std::map<std::string, GroupScan> groups;
    bool archivesScanned = false;
};
State st;

// Proxy DLL names Ultimate ASI Loader can use, plus the graphics proxies used by ReShade/OptiScaler.
const char* const kProxyNames[] = {"winmm.dll", "version.dll", "dinput8.dll", "dsound.dll", "dxgi.dll", "d3d12.dll",
                                   "d3d11.dll", "d3d9.dll", "winhttp.dll", "wininet.dll", "xinput1_3.dll", "xinput1_4.dll",
                                   "msacm32.dll", "msvfw32.dll", "binkw64.dll"};

std::string iniValue(const fs::path& ini, const std::string& sec, const std::string& key, bool& present) {
    std::string text;
    present = readFile(ini, text);
    if (!present) return "";
    qs::Ini i;
    i.parse(text);
    if (!i.has(sec, key)) return "(assente)";
    return i.get(sec, key);
}

void checkGameVersion(Report& r) {
    section("Versione del gioco");
    fs::path exe = childI(st.bin64, "CrimsonDesert.exe");
    st.exe = readExe(exe);
    if (!st.exe.readable) {
        r.add(Lv::Err, "impossibile leggere " + S(exe));
        return;
    }
    if (!st.exe.label.empty()) {
        r.add(Lv::Ok, fmt("CrimsonDesert.exe riconosciuto: %s (dimensione %llu, timestamp %u)", st.exe.label.c_str(),
                          (unsigned long long)st.exe.size, st.exe.ts));
    } else {
        std::string known;
        for (const auto& k : qs::builds::kKnownExes)
            known += fmt("\n  %s: dimensione %llu, timestamp %u", k.label, (unsigned long long)k.size, k.timestamp);
        r.add(Lv::Warn,
              fmt("CrimsonDesert.exe NON riconosciuto (dimensione %llu, timestamp %u)", (unsigned long long)st.exe.size, st.exe.ts),
              "Probabilmente il gioco è stato aggiornato. La mod si installa ma resta spenta da sola\n"
              "(controllo di versione) finché non esce una versione aggiornata di QuattroStagioni.\n"
              "Versioni supportate:" + known);
    }
    std::string paver;
    fs::path pv = childI(childI(st.game, "meta"), "0.paver");
    std::vector<uint8_t> want;
    qs::parseHex(qs::builds::kPaverHex, want);
    st.paverOk = readFile(pv, paver) && paver == std::string(want.begin(), want.end());
    if (st.paverOk) r.add(Lv::Ok, std::string("meta\\0.paver corrisponde alla versione ") + qs::builds::kGameVersion);
    else
        r.add(Lv::Warn, "meta\\0.paver diverso da " + std::string(qs::builds::kGameVersion) + " (" + (paver.empty() ? "illeggibile" : hexOf(paver)) + ")",
              "Versione dei dati del gioco diversa: la mod resterà spenta (nessun rischio per il gioco).");
}

void checkLoaders(Report& r) {
    section("Loader ASI e proxy DLL in bin64");
    int loaders = 0;
    for (const char* n : kProxyNames) {
        fs::path p = childI(st.bin64, n);
        if (!isFile(p)) continue;
        DllInfo d = classifyDll(p);
        st.proxies.push_back(d);
        if (d.asiLoader) {
            ++loaders;
            if (st.loaderName.empty()) st.loaderName = d.name;
            r.add(Lv::Ok, d.name + ": Ultimate ASI Loader");
        } else if (d.kind == "ReShade") {
            r.add(Lv::Info, d.name + ": ReShade (compatibile; se il menu F5 non compare vedi PROBLEMI.md)");
        } else if (d.kind == "OptiScaler") {
            r.add(Lv::Warn, d.name + ": OptiScaler",
                  "Con OptiScaler sono stati segnalati crash insieme ad altre mod ASI. Se il gioco si chiude\n"
                  "all'avvio, prova prima senza OptiScaler.");
        } else {
            r.add(Lv::Warn, d.name + ": proxy " + d.kind,
                  "Potrebbe essere un altro loader (ad esempio quello di un mod manager). Due loader insieme\n"
                  "fanno chiudere il gioco all'avvio: tienine uno solo.");
        }
    }
    if (loaders == 0 && opt.installLoader.empty() && isFile(st.release / "loader" / "winmm.dll") &&
        classifyDll(st.release / "loader" / "winmm.dll").asiLoader) {
        opt.installLoader = S(st.release / "loader" / "winmm.dll");  // loader shipped with the release
    }
    if (loaders == 0) {
        r.add(opt.installLoader.empty() ? Lv::Err : Lv::Info,
              opt.installLoader.empty() ? "Ultimate ASI Loader non trovato in bin64" : "Ultimate ASI Loader verrà copiato da " + opt.installLoader,
              opt.installLoader.empty()
                  ? "Scarica Ultimate ASI Loader x64 (github.com/ThirteenAG/Ultimate-ASI-Loader, file\n"
                    "Ultimate-ASI-Loader_x64.zip), rinomina dinput8.dll in winmm.dll e copialo in bin64.\n"
                    "Oppure: qs_install --install-loader \"<percorso>\\winmm.dll\". Vedi INSTALLAZIONE.md."
                  : "");
        if (st.loaderName.empty()) st.loaderName = "winmm.dll";
    } else if (loaders > 1) {
        r.add(Lv::Err, fmt("%d copie di Ultimate ASI Loader con nomi diversi", loaders),
              "Due loader insieme fanno chiudere il gioco all'avvio. Tieni solo winmm.dll e sposta gli altri\n"
              "fuori dalla cartella del gioco (non cancellarli se non sai da quale mod arrivano).");
    }
    std::string base = st.loaderName.substr(0, st.loaderName.size() - 4);
    guard.loaderIni = base + ".ini";
    bool present = false;
    fs::path ini = childI(st.bin64, guard.loaderIni);
    std::string lp = iniValue(ini, "GlobalSets", "LoadPlugins", present);
    std::string lso = iniValue(ini, "GlobalSets", "LoadFromScriptsOnly", present);
    if (!present) r.add(Lv::Info, guard.loaderIni + " assente: verrà creato");
    else if (lp == "1" && lso == "1") r.add(Lv::Ok, guard.loaderIni + ": LoadPlugins=1, LoadFromScriptsOnly=1");
    else r.add(Lv::Info, guard.loaderIni + ": LoadPlugins=" + lp + ", LoadFromScriptsOnly=" + lso + " (verranno impostati a 1, con copia di riserva)");
}

void checkOtherMods(Report& r) {
    section("Altre mod");
    bool any = false;
    for (const fs::path& f : listDir(st.bin64)) {
        std::string n = f.filename().u8string();
        if (!endsWithI(n, ".asi") || !isFile(f)) continue;
        any = true;
        if (iequals(n, "Seasons.asi"))
            r.add(Lv::Info, "bin64\\Seasons.asi (Seasons of Pywel): con LoadFromScriptsOnly=1 non viene caricato",
                  "Le due mod cambiano entrambe meteo e stagioni: non usarle insieme. Puoi lasciarlo dov'è.");
        else r.add(Lv::Info, "bin64\\" + n + ": con LoadFromScriptsOnly=1 non viene più caricato");
    }
    if (isDir(childI(st.bin64, "Seasons"))) r.add(Lv::Info, "cartella bin64\\Seasons (vecchia mod Seasons of Pywel): non verrà toccata");
    for (const char* sub : {"scripts", "plugins"}) {
        for (const fs::path& f : listDir(childI(st.bin64, sub))) {
            std::string n = f.filename().u8string();
            if (!endsWithI(n, ".asi") || iequals(n, "QuattroStagioni.asi")) continue;
            any = true;
            if (iequals(n, "Seasons.asi"))
                r.add(Lv::Warn, std::string("bin64\\") + sub + "\\Seasons.asi verrebbe caricato insieme a QuattroStagioni",
                      "Spostalo fuori da questa cartella: le due mod si contendono meteo e stagioni.");
            else r.add(Lv::Info, std::string("bin64\\") + sub + "\\" + n + ": altra mod ASI, verrà caricata insieme a QuattroStagioni");
        }
    }
    // Mod managers: folder names seen in the community (heuristic, reported as "possible").
    const char* const mm[] = {"DMM", "Definitive Mod Manager", "DefinitiveModManager", "CDUMM", "UltimateModsManager",
                              "CrimsonDesert-UltimateModsManager", "mods", "_mods", "ModManager", "mod_backup", "_backup"};
    for (const fs::path& base : {st.game, st.bin64}) {
        for (const fs::path& c : listDir(base)) {
            if (!isDir(c)) continue;
            for (const char* m : mm) {
                if (!iequals(c.filename().u8string(), m)) continue;
                any = true;
                r.add(Lv::Warn, "possibile mod manager: cartella " + S(c),
                      "Se un mod manager ha cambiato gli archivi del gioco, i pacchetti stagionali vanno generati\n"
                      "DOPO aver applicato le altre mod (o con il gioco originale: \"Revert to vanilla\").\n"
                      "Se usa un proprio loader ASI, tienine uno solo.");
            }
        }
    }
    for (const fs::path& f : listDir(st.game)) {
        if (endsWithI(f.filename().u8string(), ".cdmtl")) {
            any = true;
            r.add(Lv::Warn, "file di mod manager trovato: " + S(f));
        }
    }
    for (const char* n : {"ReShade.ini", "OptiScaler.ini", "ReShade.asi", "OptiScaler.asi"}) {
        fs::path p = childI(st.bin64, n);
        if (isFile(p)) {
            any = true;
            r.add(Lv::Info, std::string("trovato bin64\\") + n);
        }
    }
    if (!any) r.add(Lv::Ok, "nessuna altra mod ASI o mod manager rilevato");
}

void scanGroup(Report& r, const fs::path& dir, const std::string& name, int& okCount) {
    fs::path pamtPath = childI(dir, "0.pamt");
    std::string data;
    if (!readFile(pamtPath, data)) return;
    qs::Pamt pamt;
    if (!pamt.parse(reinterpret_cast<const uint8_t*>(data.data()), data.size())) {
        r.add(Lv::Warn, name + "\\0.pamt: indice non leggibile (" + pamt.error() + ")",
              "Archivio di formato diverso o modificato da un'altra mod.");
        return;
    }
    GroupScan g;
    g.name = name;
    g.selfHash = pamt.storedSelfHash();
    bool good = true;
    if (pamt.storedSelfHash() != pamt.computedSelfHash()) {
        good = false;
        r.add(Lv::Warn, fmt("%s\\0.pamt: checksum interno errato (salvato %08X, calcolato %08X)", name.c_str(),
                            pamt.storedSelfHash(), pamt.computedSelfHash()),
              "L'indice è stato modificato da un'altra mod o è danneggiato: verifica i file con Steam.");
    }
    std::set<uint32_t> listed;
    for (const qs::PamtPaz& z : pamt.pazTable()) {
        listed.insert(z.index);
        fs::path paz = childI(dir, std::to_string(z.index) + ".paz");
        uint64_t actual = isFile(paz) ? sizeOf(paz) : 0;
        g.pazSizes[z.index] = actual;
        if (!isFile(paz)) {
            good = false;
            r.add(Lv::Warn, fmt("%s\\%u.paz mancante", name.c_str(), z.index));
        } else if (actual != z.size) {
            good = false;
            r.add(Lv::Warn, fmt("%s\\%u.paz: dimensione %llu, l'indice dice %u", name.c_str(), z.index, (unsigned long long)actual, z.size),
                  "Archivio modificato (mod di texture o mod manager) o aggiornamento incompleto.");
        }
    }
    for (const fs::path& f : listDir(dir)) {
        std::string n = f.filename().u8string();
        if (!endsWithI(n, ".paz")) continue;
        char* end = nullptr;
        unsigned long idx = std::strtoul(n.c_str(), &end, 10);
        if (end && iequals(end, ".paz") && listed.count(uint32_t(idx))) continue;
        good = false;
        r.add(Lv::Warn, name + "\\" + n + ": archivio non presente nell'indice (aggiunto da un'altra mod?)");
    }
    if (good) ++okCount;
    st.groups[name] = g;
}

bool isGroupName(const std::string& n) {
    if (n.size() != 4) return false;
    for (char c : n)
        if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

void checkArchives(Report& r) {
    section("Archivi del gioco (sola lettura)");
    if (opt.noArchives) {
        r.add(Lv::Info, "controllo saltato (--no-archives)");
        return;
    }
    int ok = 0;
    for (const fs::path& d : listDir(st.game))
        if (isDir(d) && isGroupName(d.filename().u8string())) scanGroup(r, d, d.filename().u8string(), ok);
    fs::path dlc = childI(st.game, "dlc");
    for (const fs::path& pack : listDir(dlc)) {
        if (!isDir(pack)) continue;
        for (const fs::path& d : listDir(pack))
            if (isDir(d) && isGroupName(d.filename().u8string()))
                scanGroup(r, d, "dlc/" + pack.filename().u8string() + "/" + d.filename().u8string(), ok);
    }
    st.archivesScanned = true;
    if (st.groups.empty()) r.add(Lv::Warn, "nessun archivio NNNN\\0.pamt trovato: è davvero la cartella del gioco?");
    else r.add(ok == int(st.groups.size()) ? Lv::Ok : Lv::Info, fmt("%d gruppi di archivi controllati, %d integri", int(st.groups.size()), ok));
}

fs::path packsFolder() {
    fs::path folder = st.qsDir / "packs";
    std::string text;
    if (readFile(childI(st.scripts, "QuattroStagioni.ini"), text)) {
        qs::Ini ini;
        ini.parse(text);
        std::string f = ini.get("Packs", "Folder");
        if (!f.empty()) {
            std::replace(f.begin(), f.end(), '\\', '/');
            fs::path fp = P(f);
            folder = (fp.is_absolute() || (f.size() > 1 && f[1] == ':')) ? fp : st.scripts / fp;
        }
    }
    return folder;
}

void checkPacks(Report& r) {
    section("Pacchetti stagionali (creati da qs_gen sul tuo PC)");
    fs::path folder = packsFolder();
    int n = 0;
    const std::string build = qs::pack::gameBuildId(st.exe.size, st.exe.ts);
    for (const fs::path& f : listDir(folder)) {
        if (!endsWithI(f.filename().u8string(), ".qsp")) continue;
        ++n;
        qs::pack::PackReader pr;
        std::string err;
        std::string name = f.filename().u8string();
        if (!pr.openFile(f.u8string(), &err)) {
            r.add(Lv::Warn, name + ": pacchetto non valido (" + err + ")", "Cancellalo e rigeneralo con qs_gen.");
            continue;
        }
        std::string why;
        if (pr.header().gameBuild != build) why = "creato per un'altra versione dell'eseguibile";
        if (why.empty() && st.archivesScanned) {
            for (const auto& g : pr.header().pamtHashes) {
                auto it = st.groups.find(g.first);
                if (it == st.groups.end() || it->second.selfHash != g.second) {
                    why = "l'indice " + g.first + "\\0.pamt è cambiato";
                    break;
                }
            }
            for (const qs::pack::PazInfo& z : pr.paz()) {
                if (!why.empty()) break;
                auto it = st.groups.find(z.group);
                uint64_t actual = it == st.groups.end() || !it->second.pazSizes.count(z.pazIndex) ? 0 : it->second.pazSizes.at(z.pazIndex);
                if (actual != z.pazSize) why = "l'archivio " + z.key() + " ha una dimensione diversa";
            }
        }
        if (why.empty())
            r.add(Lv::Ok, fmt("%s: stadio %s, %zu texture, corrisponde al gioco installato", name.c_str(),
                              pr.header().stageId.c_str(), pr.records().size()));
        else
            r.add(Lv::Warn, name + ": da rigenerare (" + why + ")",
                  "Il plugin lo rifiuterà e userà le texture originali. Rigeneralo con qs_gen.");
    }
    if (n == 0)
        r.add(Lv::Info, "nessun pacchetto in " + S(folder),
              "Senza pacchetti la mod funziona (calendario, menu F5) ma le piante non cambiano colore.\n"
              "Generali con qs_gen dopo l'installazione: vedi INSTALLAZIONE.md, passo 5.");
}

// ------------------------------------------------------------------ manifest
struct Entry {
    std::string rel;    // relative to bin64, forward slashes
    std::string state;  // new | replaced | kept
    std::string shaInstalled = "-", shaOriginal = "-", backup = "-";
};
struct Manifest {
    std::string version, date, game, loaderIni = "winmm.ini";
    std::vector<Entry> files;
    std::vector<std::string> dirs;  // created by the installer, parents first
    std::vector<std::string> pre;   // QuattroStagioni files that existed before the first install
    std::vector<std::string> predirs;  // and the folders under bin64\scripts\QuattroStagioni
    Entry* find(const std::string& rel) {
        for (Entry& e : files)
            if (iequals(e.rel, rel)) return &e;
        return nullptr;
    }
};

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> v;
    size_t s = 0;
    while (true) {
        size_t t = line.find('\t', s);
        v.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
        if (t == std::string::npos) break;
        s = t + 1;
    }
    return v;
}

bool loadManifest(const fs::path& p, Manifest& m) {
    std::string text;
    if (!readFile(p, text)) return false;
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find('\n', s);
        std::string line = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
        s = e == std::string::npos ? text.size() : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f = splitTabs(line);
        if (f[0] == "version" && f.size() > 1) m.version = f[1];
        else if (f[0] == "date" && f.size() > 1) m.date = f[1];
        else if (f[0] == "game" && f.size() > 1) m.game = f[1];
        else if (f[0] == "loader_ini" && f.size() > 1) m.loaderIni = f[1];
        else if (f[0] == "dir" && f.size() > 1) m.dirs.push_back(f[1]);
        else if (f[0] == "pre" && f.size() > 1) m.pre.push_back(f[1]);
        else if (f[0] == "predir" && f.size() > 1) m.predirs.push_back(f[1]);
        else if (f[0] == "file" && f.size() >= 6) m.files.push_back(Entry{f[1], f[2], f[3], f[4], f[5]});
    }
    return true;
}

std::string manifestText(const Manifest& m) {
    std::string t = std::string(kManifestHeader) + "\r\n";
    t += "version\t" + m.version + "\r\ndate\t" + m.date + "\r\ngame\t" + m.game + "\r\nloader_ini\t" + m.loaderIni + "\r\n";
    for (const std::string& d : m.dirs) t += "dir\t" + d + "\r\n";
    for (const std::string& p : m.pre) t += "pre\t" + p + "\r\n";
    for (const std::string& p : m.predirs) t += "predir\t" + p + "\r\n";
    for (const Entry& e : m.files)
        t += "file\t" + e.rel + "\t" + e.state + "\t" + e.shaInstalled + "\t" + e.shaOriginal + "\t" + e.backup + "\r\n";
    return t;
}

// QuattroStagioni files present now (relative to bin64): bin64\scripts\QuattroStagioni*.* and the
// whole bin64\scripts\QuattroStagioni folder except the installer's own folder.
std::vector<std::string> quattroFiles() {
    std::vector<std::string> v;
    for (const fs::path& f : listDir(st.scripts))
        if (isFile(f) && lower(f.filename().u8string()).rfind("quattrostagioni", 0) == 0)
            v.push_back(relKey(f.lexically_relative(st.bin64)));
    std::error_code ec;
    if (isDir(st.qsDir)) {
        for (fs::recursive_directory_iterator it(st.qsDir, ec), end; !ec && it != end; it.increment(ec)) {
            fs::path rel = it->path().lexically_relative(st.qsDir);
            if (!rel.empty() && iequals(rel.begin()->u8string(), "install")) continue;
            if (it->is_regular_file(ec)) v.push_back(relKey(it->path().lexically_relative(st.bin64)));
        }
    }
    std::sort(v.begin(), v.end());
    return v;
}

// Folders under bin64\scripts\QuattroStagioni (except the installer's own), relative to bin64.
std::vector<std::string> quattroDirs() {
    std::vector<std::string> v;
    std::error_code ec;
    if (!isDir(st.qsDir)) return v;
    for (fs::recursive_directory_iterator it(st.qsDir, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path rel = it->path().lexically_relative(st.qsDir);
        if (!rel.empty() && iequals(rel.begin()->u8string(), "install")) continue;
        if (it->is_directory(ec)) v.push_back(relKey(it->path().lexically_relative(st.bin64)));
    }
    std::sort(v.begin(), v.end());
    return v;
}

// ------------------------------------------------------------------ install
struct Planned {
    fs::path src;
    std::string rel;  // destination relative to bin64
    std::string sha;
    bool config = false;
};

std::vector<Planned> planFiles(std::string& err) {
    std::vector<Planned> v;
    fs::path base = st.release / "bin64" / "scripts";
    if (!isFile(base / "QuattroStagioni.asi")) {
        err = "la cartella della release non contiene bin64\\scripts\\QuattroStagioni.asi: " + S(st.release);
        return v;
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        Planned p;
        p.src = it->path();
        p.rel = "scripts/" + relKey(it->path().lexically_relative(base));
        if (!sha256File(p.src, p.sha)) {
            err = "impossibile leggere " + S(p.src);
            return {};
        }
        p.config = endsWithI(p.rel, ".ini") && !endsWithI(p.rel, "manifest.ini");
        v.push_back(p);
    }
    std::sort(v.begin(), v.end(), [](const Planned& a, const Planned& b) { return a.rel < b.rel; });
    return v;
}

// [GlobalSets] LoadPlugins=1 and LoadFromScriptsOnly=1, everything else kept byte for byte.
std::string editLoaderIni(const std::string& in) {
    std::string eol = in.find("\r\n") != std::string::npos || in.empty() ? "\r\n" : "\n";
    std::vector<std::string> lines;
    size_t s = 0;
    while (s < in.size()) {
        size_t e = in.find('\n', s);
        std::string l = in.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        s = e == std::string::npos ? in.size() : e + 1;
    }
    auto trim = [](std::string x) {
        size_t a = x.find_first_not_of(" \t"), b = x.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
    };
    const char* keys[] = {"LoadPlugins", "LoadFromScriptsOnly"};
    bool done[2] = {false, false};
    int secStart = -1, secEnd = int(lines.size());
    for (int i = 0; i < int(lines.size()); ++i) {
        std::string t = trim(lines[i]);
        if (!t.empty() && t[0] == '[') {
            if (secStart >= 0 && secEnd == int(lines.size())) secEnd = i;
            if (iequals(t, "[GlobalSets]") && secStart < 0) secStart = i;
            continue;
        }
        if (secStart < 0 || (secEnd != int(lines.size()) && i >= secEnd)) continue;
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        for (int j = 0; j < 2; ++j) {
            if (!iequals(k, keys[j])) continue;
            std::string v = trim(t.substr(eq + 1));
            size_t sc = v.find(';');
            if (sc != std::string::npos) v = trim(v.substr(0, sc));
            if (v != "1") lines[i] = std::string(keys[j]) + "=1";
            done[j] = true;
        }
    }
    if (secStart < 0) {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back("[GlobalSets]");
        secStart = int(lines.size()) - 1;
        secEnd = int(lines.size());
    }
    int insertAt = secStart + 1;
    for (int j = 0; j < 2; ++j)
        if (!done[j]) lines.insert(lines.begin() + insertAt++, std::string(keys[j]) + "=1");
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || (!in.empty() && (in.back() == '\n')) || in.empty()) out += eol;
    }
    return out;
}

const char* const kDefaultLoaderIni =
    "[GlobalSets]\r\nLoadPlugins=1\r\nLoadFromScriptsOnly=1\r\nDontLoadFromDllMain=1\r\nFindModule=0\r\n";

// Puts one manifest entry back to its pre-install state. Returns false on a problem (logged).
bool revertEntry(Report& r, const Entry& e) {
    fs::path dst = st.bin64 / P(e.rel);
    if (e.state == "kept") return true;
    if (e.state == "new") {
        if (!pathExists(dst)) return true;
        std::string cur;
        if (sha256File(dst, cur) && cur != e.shaInstalled && e.shaInstalled != "-")
            r.add(Lv::Info, e.rel + ": modificato dopo l'installazione (impostazioni salvate dal menu?), rimosso comunque");
        if (!safeRemoveFile(dst)) {
            r.add(Lv::Err, "impossibile cancellare " + S(dst), "Il gioco è aperto? Chiudilo e riprova.");
            return false;
        }
        return true;
    }
    if (e.state == "replaced") {
        fs::path bak = st.installDir / P(e.backup);
        std::string bsha;
        if (!sha256File(bak, bsha) || bsha != e.shaOriginal) {
            r.add(Lv::Err, "copia di riserva di " + e.rel + " mancante o alterata: " + S(bak), "Il file attuale resta dov'è.");
            return false;
        }
        std::string err;
        if (!safeCopyVerified(bak, dst, e.shaOriginal, err)) {
            r.add(Lv::Err, "ripristino di " + e.rel + " non riuscito: " + err);
            return false;
        }
        return true;
    }
    r.add(Lv::Err, "voce del manifest sconosciuta: " + e.state + " " + e.rel);
    return false;
}

bool confirm(const std::string& question) {
    if (opt.yes) return true;
    if (!QS_ISATTY(stdin)) {
        say("%s", "Esecuzione non interattiva: aggiungi --yes per confermare.");
        return false;
    }
    std::printf("%s [s/N] ", question.c_str());
    std::fflush(stdout);
    char buf[16] = {0};
    if (!std::fgets(buf, sizeof buf, stdin)) return false;
    return buf[0] == 's' || buf[0] == 'S' || buf[0] == 'y' || buf[0] == 'Y';
}

void appendLog(const fs::path& p) {
    if (!isDir(p.parent_path())) return;
    std::ofstream f(p, std::ios::binary | std::ios::app);
    if (f) f << "\r\n==== " << nowText() << " " << kToolVersion << "\r\n" << con.log;
}

int doInstall(Report& r) {
    std::string err;
    std::vector<Planned> plan = planFiles(err);
    if (plan.empty()) {
        r.add(Lv::Err, err);
        return 1;
    }
    Manifest old;
    const bool upgrade = loadManifest(st.manifestPath, old);
    section(upgrade ? "Aggiornamento (installazione precedente: " + old.version + " del " + old.date + ")" : "Installazione");
    say("  Da:  %s", S(st.release).c_str());
    say("  A:   %s", S(st.scripts).c_str());
    for (const Planned& p : plan) say("       %s", p.rel.c_str());
    say("  E:   %s (LoadPlugins=1, LoadFromScriptsOnly=1)", S(st.bin64 / P(guard.loaderIni)).c_str());
    if (!opt.installLoader.empty()) say("  E:   %s (Ultimate ASI Loader)", S(st.bin64 / P(guard.loaderDll)).c_str());
    say("  I file del gioco non vengono toccati. Copie di riserva in %s", S(st.installDir / "backup").c_str());
    if (r.errors > 0) {
        say("%s", "");
        say("%s", "Ci sono errori da correggere prima (vedi sopra): installazione annullata.");
        return 1;
    }
    if (!confirm("Procedere con l'installazione?")) {
        say("%s", "Installazione annullata, nessun file modificato.");
        return 1;
    }

    Manifest m;
    m.version = st.releaseVersion;
    m.date = nowText();
    m.game = S(st.game);
    m.loaderIni = guard.loaderIni;
    if (upgrade) {
        m.dirs = old.dirs;
        m.pre = old.pre;
        m.predirs = old.predirs;
        if (!iequals(old.loaderIni, guard.loaderIni)) {
            r.add(Lv::Warn, "il loader è cambiato (" + old.loaderIni + " -> " + guard.loaderIni + ")");
        }
    } else {
        m.pre = quattroFiles();
        m.predirs = quattroDirs();
    }

    std::vector<fs::path> created;
    std::vector<Entry> doneNew;  // for rollback of a first install
    auto rollback = [&]() {
        if (upgrade) {
            r.add(Lv::Err, "installazione interrotta: rilancia qs_install oppure qs_install --uninstall");
            return;
        }
        say("%s", "  Annullamento delle modifiche fatte finora...");
        bool reverted = true;
        try {
            for (auto it = doneNew.rbegin(); it != doneNew.rend(); ++it) reverted &= revertEntry(r, *it);
        } catch (const std::exception& ex) {
            r.add(Lv::Err, ex.what());
            reverted = false;
        }
        if (!reverted) {  // keep the backups: they are the only copy of the replaced files
            r.add(Lv::Err, "annullamento incompleto: le copie di riserva restano in " + S(st.installDir / "backup"));
            return;
        }
        std::error_code ec;
        if (isDir(st.installDir)) {
            guard.check(st.installDir, "delete");
            fs::remove_all(st.installDir, ec);
        }
        for (auto it = created.rbegin(); it != created.rend(); ++it) safeRemoveEmptyDir(*it);
    };

    try {
        auto mkdirs = [&](const fs::path& d) {
            std::vector<fs::path> c;
            if (!safeMkdirs(d, &c)) throw std::runtime_error("impossibile creare la cartella " + S(d));
            for (const fs::path& x : c) {
                created.push_back(x);
                m.dirs.push_back(relKey(x.lexically_relative(st.bin64)));
            }
        };
        mkdirs(st.installDir / "backup");
        auto backupOf = [&](const std::string& rel, const fs::path& dst, Entry& e) {
            e.backup = "backup/" + rel;
            fs::path bak = st.installDir / P(e.backup);
            mkdirs(bak.parent_path());
            if (!sha256File(dst, e.shaOriginal)) throw std::runtime_error("impossibile leggere " + S(dst));
            std::string err2;
            if (!safeCopyVerified(dst, bak, e.shaOriginal, err2)) throw std::runtime_error("copia di riserva di " + rel + ": " + err2);
        };

        std::set<std::string> newSet;
        for (const Planned& p : plan) {
            newSet.insert(lower(p.rel));
            fs::path dst = st.bin64 / P(p.rel);
            mkdirs(dst.parent_path());
            Entry* prev = upgrade ? old.find(p.rel) : nullptr;
            Entry e;
            e.rel = p.rel;
            if (prev) e = *prev;
            std::string cur;
            const bool there = isFile(dst) && sha256File(dst, cur);
            bool writeNewCopy = false;
            if (there && p.config && !opt.resetConfig && cur != p.sha &&
                (prev ? (prev->state == "kept" || cur != prev->shaInstalled) : true)) {
                // the player's settings: keep them, put the new defaults next to them
                writeNewCopy = true;
                if (!prev) e.state = "kept";
                r.add(Lv::Info, p.rel + ": tengo le tue impostazioni, i nuovi valori predefiniti sono in " + p.rel + ".new");
            } else if (!prev) {
                if (there) {
                    e.state = "replaced";
                    backupOf(p.rel, dst, e);
                } else {
                    e.state = "new";
                }
            } else if (prev->state == "kept") {
                // was kept before, now overwritten (--reset-config or identical): back it up first
                e.state = "replaced";
                backupOf(p.rel, dst, e);
            }
            if (writeNewCopy) {
                std::string nrel = p.rel + ".new";
                fs::path ndst = st.bin64 / P(nrel);
                Entry* pn = upgrade ? old.find(nrel) : nullptr;
                Entry ne = pn ? *pn : Entry{nrel, "new"};
                if (!pn && isFile(ndst)) {
                    ne.state = "replaced";
                    backupOf(nrel, ndst, ne);
                }
                if (!pn) doneNew.push_back(ne);  // before writing: a failed copy is rolled back too
                std::string err2;
                if (!safeCopyVerified(p.src, ndst, p.sha, err2)) throw std::runtime_error(nrel + ": " + err2);
                ne.shaInstalled = p.sha;
                newSet.insert(lower(nrel));
                m.files.push_back(ne);
                e.shaInstalled = prev ? prev->shaInstalled : "-";
            } else {
                if (!prev) doneNew.push_back(e);
                if (!(there && cur == p.sha)) {
                    std::string err2;
                    if (!safeCopyVerified(p.src, dst, p.sha, err2)) throw std::runtime_error(p.rel + ": " + err2);
                }
                e.shaInstalled = p.sha;
            }
            m.files.push_back(e);
            if (!prev && writeNewCopy) doneNew.push_back(e);
            say("  [OK]     %s%s", p.rel.c_str(), writeNewCopy ? " (tenuto il tuo, nuovo in .new)" : " (SHA-256 verificato)");
        }
        // loader ini
        {
            const std::string rel = guard.loaderIni;
            fs::path dst = st.bin64 / P(rel);
            Entry* prev = upgrade ? old.find(rel) : nullptr;
            Entry e = prev ? *prev : Entry{rel, "new"};
            std::string cur, content;
            const bool there = isFile(dst) && readFile(dst, cur);
            if (!prev && there) {
                e.state = "replaced";
                backupOf(rel, dst, e);
            }
            if (there) {
                content = editLoaderIni(cur);
            } else {
                std::string tmpl;
                content = readFile(st.release / "bin64" / "winmm.ini", tmpl) && !tmpl.empty() ? editLoaderIni(tmpl) : kDefaultLoaderIni;
            }
            if (!prev) doneNew.push_back(e);
            if (!there || content != cur) {
                std::string err2;
                if (!safeWriteVerified(dst, content, err2)) throw std::runtime_error(rel + ": " + err2);
            }
            e.shaInstalled = sha256Bytes(content);
            newSet.insert(lower(rel));
            m.files.push_back(e);
            say("  [OK]     %s: LoadPlugins=1, LoadFromScriptsOnly=1%s", rel.c_str(), e.state == "replaced" && !prev ? " (originale salvato)" : "");
        }
        // optional loader dll
        if (!opt.installLoader.empty()) {
            fs::path src = P(opt.installLoader);
            const std::string rel = guard.loaderDll;
            fs::path dst = st.bin64 / P(rel);
            DllInfo d = classifyDll(src);
            if (!d.asiLoader) throw std::runtime_error(opt.installLoader + " non sembra Ultimate ASI Loader");
            if (isFile(dst) && !(upgrade && old.find(rel)))
                throw std::runtime_error(S(dst) + " esiste già: non lo sovrascrivo");
            Entry* prev = upgrade ? old.find(rel) : nullptr;
            Entry e = prev ? *prev : Entry{rel, "new"};
            std::string sha;
            if (!sha256File(src, sha)) throw std::runtime_error("impossibile leggere " + opt.installLoader);
            if (!prev) doneNew.push_back(e);
            std::string err2;
            if (!safeCopyVerified(src, dst, sha, err2)) throw std::runtime_error(rel + ": " + err2);
            e.shaInstalled = sha;
            newSet.insert(lower(rel));
            m.files.push_back(e);
            say("  [OK]     %s: Ultimate ASI Loader (SHA-256 verificato)", rel.c_str());
        }
        // files of the previous version that this release no longer has
        if (upgrade) {
            for (const Entry& e : old.files) {
                if (newSet.count(lower(e.rel))) continue;
                if (revertEntry(r, e)) say("  [OK]     %s: non più usato, rimosso/ripristinato", e.rel.c_str());
                else m.files.push_back(e);
            }
        }
        std::string err2;
        if (!safeWriteVerified(st.manifestPath, manifestText(m), err2)) throw std::runtime_error("manifest: " + err2);
    } catch (const std::exception& ex) {
        r.add(Lv::Err, ex.what());
        rollback();
        return 1;
    }
    section("Fatto");
    say("  QuattroStagioni %s installato. Disinstallazione completa: qs_install --uninstall", st.releaseVersion.c_str());
    say("%s", "  Prossimi passi: genera i pacchetti stagionali con qs_gen (INSTALLAZIONE.md, passo 5),");
    say("%s", "  avvia il gioco e premi F5 per il menu. Il log è in bin64\\scripts\\QuattroStagioni.log");
    appendLog(st.installDir / "qs_install.log");
    return 0;
}

int doUninstall(Report& r) {
    section("Disinstallazione");
    Manifest m;
    if (!loadManifest(st.manifestPath, m)) {
        r.add(Lv::Err, "nessuna installazione di qs_install trovata (manca " + S(st.manifestPath) + ")",
              "Se avevi installato a mano: cancella bin64\\scripts\\QuattroStagioni.asi, QuattroStagioni.ini,\n"
              "la cartella bin64\\scripts\\QuattroStagioni e rimetti LoadPlugins=0 in bin64\\winmm.ini.");
        return 1;
    }
    guard.loaderIni = m.loaderIni;
    for (const Entry& e : m.files)
        if (endsWithI(e.rel, ".dll") && e.rel.find('/') == std::string::npos) guard.loaderDll = e.rel;
    say("  Installazione del %s, versione %s", m.date.c_str(), m.version.c_str());
    for (const Entry& e : m.files) {
        if (e.state == "kept") continue;
        say("       %s %s", e.state == "new" ? "rimuovo   " : "ripristino", e.rel.c_str());
    }
    say("  %s", opt.purge ? "Anche i pacchetti stagionali generati verranno cancellati (--purge)."
                          : "I pacchetti stagionali generati (.qsp) restano: aggiungi --purge per cancellarli.");
    if (!confirm("Procedere con la disinstallazione?")) {
        say("%s", "Disinstallazione annullata, nessun file modificato.");
        return 1;
    }
    bool ok = true;
    try {
        for (auto it = m.files.rbegin(); it != m.files.rend(); ++it) {
            bool done = revertEntry(r, *it);
            ok &= done;
            if (done && it->state != "kept") say("  [OK]     %s %s", it->rel.c_str(), it->state == "new" ? "rimosso" : "ripristinato (SHA-256 verificato)");
        }
        // files the plugin and qs_gen created while playing (log, state.ini, packs...)
        std::set<std::string> pre;
        for (const std::string& p : m.pre) pre.insert(lower(p));
        fs::path packs = packsFolder();
        int keptPacks = 0;
        for (const std::string& rel : quattroFiles()) {
            if (pre.count(lower(rel))) continue;
            fs::path p = st.bin64 / P(rel);
            fs::path prel = p.lexically_relative(packs);
            bool inPacks = !prel.empty() && prel.begin()->u8string() != "..";
            if (inPacks && !opt.purge) {
                ++keptPacks;
                continue;
            }
            if (safeRemoveFile(p)) say("  [OK]     %s rimosso (creato dalla mod durante il gioco)", rel.c_str());
            else {
                r.add(Lv::Err, "impossibile cancellare " + S(p));
                ok = false;
            }
        }
        // empty folders created while playing (packs\ by qs_gen...), deepest first
        {
            std::set<std::string> predirs;
            for (const std::string& d : m.predirs) predirs.insert(lower(d));
            std::vector<std::string> dirs = quattroDirs();
            std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
            for (const std::string& d : dirs)
                if (!predirs.count(lower(d))) safeRemoveEmptyDir(st.bin64 / P(d));
        }
        if (keptPacks) r.add(Lv::Info, fmt("%d file nella cartella dei pacchetti lasciati al loro posto: %s", keptPacks, S(packs).c_str()));
        if (ok) {
            // the installer's own folder (manifest, backups, log), then the folders it created
            std::error_code ec;
            guard.check(st.installDir, "delete");
            fs::remove_all(st.installDir, ec);
            if (ec) {
                r.add(Lv::Err, "impossibile cancellare " + S(st.installDir));
                ok = false;
            }
            std::vector<std::string> dirs = m.dirs;
            std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
            for (const std::string& d : dirs) {
                fs::path p = st.bin64 / P(d);
                if (isDir(p) && !safeRemoveEmptyDir(p) && !iequals(d, "scripts/QuattroStagioni/install"))
                    r.add(Lv::Info, "cartella non vuota lasciata: " + S(p));
            }
        } else {
            r.add(Lv::Err, "qualcosa non è andato: le copie di riserva restano in " + S(st.installDir / "backup"),
                  "Chiudi il gioco e rilancia qs_install --uninstall.");
        }
    } catch (const std::exception& ex) {
        r.add(Lv::Err, ex.what());
        ok = false;
    }
    section(ok ? "Fatto" : "Disinstallazione incompleta");
    if (ok) say("%s", "  QuattroStagioni rimosso: bin64 è tornato com'era prima dell'installazione.");
    appendLog(st.exeDir / "qs_uninstall.log");
    return ok ? 0 : 1;
}

void printHelp() {
    say("%s", kToolVersion);
    say("%s", "Installa, controlla o rimuove QuattroStagioni (Crimson Desert). Non modifica mai i file del gioco.");
    say("%s", "");
    say("%s", "  qs_install                 installa (chiede conferma)");
    say("%s", "  qs_install --check         controllo senza modifiche: versione, loader, altre mod, archivi, pacchetti");
    say("%s", "  qs_install --uninstall     rimette tutto com'era prima dell'installazione");
    say("%s", "");
    say("%s", "Opzioni:");
    say("%s", "  --game \"<cartella>\"        cartella di Crimson Desert (quella che contiene bin64)");
    say("%s", "  --from \"<cartella>\"        cartella della release (predefinita: quella di qs_install.exe)");
    say("%s", "  --steam \"<cartella>\"       cartella di Steam, se non è quella standard");
    say("%s", "  --yes                      non chiedere conferma");
    say("%s", "  --purge                    con --uninstall: cancella anche i pacchetti stagionali generati");
    say("%s", "  --reset-config             sovrascrive QuattroStagioni.ini modificato (copia di riserva inclusa)");
    say("%s", "  --install-loader \"<file>\"  copia anche Ultimate ASI Loader (winmm.dll) in bin64");
    say("%s", "  --no-archives              salta il controllo degli archivi del gioco (più veloce)");
    say("%s", "  --report \"<file>\"          salva anche il testo del controllo in un file");
    say("%s", "  --print-game               stampa solo la cartella del gioco trovata (per gli script)");
}

bool parseArgs(const std::vector<std::string>& a, std::string& err) {
    for (size_t i = 1; i < a.size(); ++i) {
        const std::string& k = a[i];
        auto val = [&](std::string& out) {
            if (i + 1 >= a.size()) {
                err = k + " richiede un valore";
                return false;
            }
            out = a[++i];
            return true;
        };
        if (k == "--check" || k == "--dry-run") opt.mode = Options::Check;
        else if (k == "--uninstall") opt.mode = Options::Uninstall;
        else if (k == "--help" || k == "-h" || k == "/?") opt.mode = Options::Help;
        else if (k == "--version") opt.mode = Options::Version;
        else if (k == "--print-game") { opt.mode = Options::PrintGame; opt.noPause = true; }
        else if (k == "--game") { if (!val(opt.game)) return false; }
        else if (k == "--from") { if (!val(opt.from)) return false; }
        else if (k == "--steam") { if (!val(opt.steam)) return false; }
        else if (k == "--install-loader") { if (!val(opt.installLoader)) return false; }
        else if (k == "--report") { if (!val(opt.report)) return false; }
        else if (k == "--yes" || k == "-y") opt.yes = true;
        else if (k == "--purge") opt.purge = true;
        else if (k == "--reset-config") opt.resetConfig = true;
        else if (k == "--no-archives") opt.noArchives = true;
        else if (k == "--no-pause") opt.noPause = true;
        else if (k == "--test-build") {
            std::string v;
            if (!val(v)) return false;
            unsigned long long size = 0;
            unsigned long ts = 0;
            if (std::sscanf(v.c_str(), "%llu:%lu", &size, &ts) != 2) {
                err = "--test-build vuole <dimensione>:<timestamp>";
                return false;
            }
            opt.testBuilds.push_back({size, uint32_t(ts), "build di prova (--test-build)"});
        } else {
            err = "opzione sconosciuta: " + k + " (vedi --help)";
            return false;
        }
    }
    return true;
}

std::vector<std::string> utf8Args(int argc, char** argv) {
    std::vector<std::string> a;
#ifdef _WIN32
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (w) {
        for (int i = 0; i < n; ++i) a.push_back(fs::path(std::wstring(w[i])).u8string());
        LocalFree(w);
        return a;
    }
#endif
    for (int i = 0; i < argc; ++i) a.push_back(argv[i]);
    return a;
}

fs::path selfDir(const std::string& argv0) {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    if (n > 0 && n < 4096) return fs::path(std::wstring(buf, n)).parent_path();
#endif
    std::error_code ec;
    fs::path p = fs::absolute(P(argv0), ec);
    return p.parent_path();
}

void pauseIfDoubleClicked() {
#ifdef _WIN32
    if (opt.noPause) return;
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) {  // our own console window: keep it open
        std::printf("\nPremi Invio per chiudere...");
        std::fflush(stdout);
        std::getchar();
    }
#endif
}

int run(int argc, char** argv) {
    std::vector<std::string> args = utf8Args(argc, argv);
    std::string err;
    if (!parseArgs(args, err)) {
        say("qs_install: %s", err.c_str());
        return 2;
    }
    if (opt.mode == Options::Help) {
        printHelp();
        return 0;
    }
    if (opt.mode == Options::Version) {
        say("%s", kToolVersion);
        return 0;
    }
    st.exeDir = selfDir(args.empty() ? "qs_install" : args[0]);
    st.release = opt.from.empty() ? st.exeDir : fs::absolute(P(opt.from));
    if (opt.mode == Options::PrintGame) {  // for genera_pacchetti.bat: the game folder, nothing else
        fs::path g;
        if (!opt.game.empty()) g = fs::absolute(P(opt.game));
        else {
            std::vector<fs::path> found = findGameFolders(st.exeDir);
            if (!found.empty()) g = found[0];
        }
        if (g.empty() || !isGameRoot(g)) return 1;
        std::printf("%s\n", S(g).c_str());
        return 0;
    }
    std::string ver;
    if (readFile(st.release / "VERSION", ver)) {
        size_t nl = ver.find_first_of("\r\n");
        st.releaseVersion = ver.substr(0, nl);
    }
    say("QuattroStagioni - %s (release %s)", kToolVersion, st.releaseVersion.c_str());
    say("%s", opt.mode == Options::Check ? "Modalità controllo: nessun file verrà modificato."
                                         : opt.mode == Options::Uninstall ? "Modalità disinstallazione." : "Modalità installazione.");
    Report r;
    section("Cartella del gioco");
    if (!opt.game.empty()) {
        st.game = fs::absolute(P(opt.game));
        if (!isGameRoot(st.game)) {
            r.add(Lv::Err, "in " + S(st.game) + " non c'è bin64\\CrimsonDesert.exe",
                  "Indica la cartella che CONTIENE bin64, ad esempio\n"
                  "\"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Crimson Desert\".");
            return 1;
        }
        r.add(Lv::Ok, S(st.game) + " (indicata con --game)");
    } else {
        std::vector<fs::path> found = findGameFolders(st.exeDir);
        if (found.empty()) {
            r.add(Lv::Err, "Crimson Desert non trovato automaticamente",
                  "Indica la cartella con: qs_install --game \"<cartella di Crimson Desert>\"\n"
                  "(in Steam: tasto destro sul gioco, Gestisci, Sfoglia i file locali).");
            return 1;
        }
        st.game = found[0];
        r.add(Lv::Ok, S(st.game) + " (trovata tramite Steam)");
        for (size_t i = 1; i < found.size(); ++i) r.add(Lv::Info, "altra copia trovata: " + S(found[i]) + " (usa --game per sceglierla)");
    }
    st.bin64 = childI(st.game, "bin64");
    st.scripts = childI(st.bin64, "scripts");
    st.qsDir = childI(st.scripts, "QuattroStagioni");
    st.installDir = st.qsDir / "install";
    st.manifestPath = st.installDir / "uninstall.txt";
    guard.bin64 = st.bin64;
    if (!opt.installLoader.empty()) guard.loaderDll = "winmm.dll";

    if (opt.mode == Options::Uninstall) {
        int rc = doUninstall(r);
        return rc;
    }

    checkGameVersion(r);
    checkLoaders(r);
    if (!opt.installLoader.empty()) {
        guard.loaderDll = st.loaderName.empty() ? "winmm.dll" : st.loaderName;
    }
    checkOtherMods(r);
    checkArchives(r);
    checkPacks(r);
    section("Installazione esistente");
    Manifest old;
    if (loadManifest(st.manifestPath, old)) r.add(Lv::Info, "QuattroStagioni " + old.version + " installato il " + old.date + " con qs_install");
    else if (isFile(childI(st.scripts, "QuattroStagioni.asi")))
        r.add(Lv::Info, "QuattroStagioni.asi presente (installato a mano): i file attuali verranno salvati come copia di riserva");
    else r.add(Lv::Info, "nessuna installazione precedente");
    section("Antivirus");
    r.add(Lv::Info, "Windows Defender a volte mette in quarantena i file .asi (falso positivo).",
          "Se QuattroStagioni.asi sparisce o il log non viene creato, aggiungi un'esclusione per\n" + S(st.scripts) +
              "\n(Sicurezza di Windows, Protezione da virus e minacce, Esclusioni). Vedi PROBLEMI.md.");

    int rc = 0;
    if (opt.mode == Options::Check) {
        std::string perr;
        std::vector<Planned> plan = planFiles(perr);
        section("Riepilogo");
        say("  %d errori, %d avvisi.", r.errors, r.warnings);
        if (plan.empty()) say("  Installazione: %s", perr.c_str());
        else say("  L'installazione copierebbe %zu file in %s e imposterebbe %s.", plan.size(), S(st.scripts).c_str(), guard.loaderIni.c_str());
        say("%s", r.errors ? "  Correggi gli errori prima di installare." : "  Si può installare.");
        rc = r.errors ? 3 : 0;
    } else {
        rc = doInstall(r);
    }
    if (!opt.report.empty()) {
        std::ofstream f(P(opt.report), std::ios::binary);
        if (f) f << con.log;
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    int rc;
    try {
        rc = run(argc, argv);
    } catch (const std::exception& ex) {
        say("qs_install: errore inatteso: %s (nessun file del gioco è stato toccato)", ex.what());
        rc = 1;
    }
    pauseIfDoubleClicked();
    return rc;
}
