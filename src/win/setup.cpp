// QuattroStagioni - version guard and substitution loading.
//
// Everything here only READS game files. The checks, in order:
//   1. meta/0.paver equals the 2.04.00 bytes baked below;
//   2. bin64/CrimsonDesert.exe has the baked file size and PE TimeDateStamp;
//   3. per manifest entry: <group>/0.pamt self-hash is intact and equals the manifest value, the
//      entry record (paz index, offset, sizes, flags) matches, the .paz size matches the pamt paz
//      table, the original stored bytes hash to the manifest values, the replacement file has the
//      same length and its own hashes, and for per-mip DDS the 128-byte header (with the mip stored
//      sizes that .pathc also records) is identical.
// Steps 1-2 failing disables everything; step 3 failing disables only that entry.
// Season packs (.qsp from qs_gen) are checked by loadVerifiedPack: integrity, game build, pamt hashes
// and paz sizes; original bytes are verified per read by the pack server (core/pack/serve.h).
#include "setup.h"

#include "known_builds.h"

#include <windows.h>

#include <cstring>
#include <map>

#include "../core/dds.h"
#include "../core/ini.h"
#include "../core/lookup3.h"
#include "../core/lz4_exact.h"
#include "../core/gen/stage.h"
#include "../core/manifest.h"
#include "../core/pack/qsp_io.h"
#include "../core/pamt.h"
#include "log.h"
#include "winutil.h"

namespace qs {
namespace {

// Baked fingerprint of the supported game builds: single list shared with qs_install.
const char* const kPaver204Hex = builds::kPaverHex;
const uint64_t kExeSize204 = builds::kLatestExe.size;
const uint32_t kExeTimestamp204 = builds::kLatestExe.timestamp;
const char* const kGameVersion = builds::kGameVersion;

bool readPeTimestamp(const std::wstring& exe, uint32_t& ts) {
    std::vector<uint8_t> h;
    if (!readFileRange(exe, 0, 4096, h)) return false;
    if (h[0] != 'M' || h[1] != 'Z') return false;
    uint32_t pe;
    std::memcpy(&pe, &h[0x3C], 4);
    if (pe + 12 > h.size() || std::memcmp(&h[pe], "PE\0\0", 4) != 0) return false;
    std::memcpy(&ts, &h[pe + 8], 4);
    return true;
}

bool loadEntry(const ManifestEntry& e, const std::wstring& root, const std::wstring& manifestDir,
               ArchiveTarget& target, Substitution& out) {
    const char* id = e.id.c_str();
    // ---- index cross-check -------------------------------------------------------------------
    std::wstring pamtPath = joinPath(joinPath(root, widen(e.group)), L"0.pamt");
    std::vector<uint8_t> pamtData;
    if (!readWholeFile(pamtPath, pamtData)) { QS_ERR("[%s] cannot read %s", id, narrow(pamtPath).c_str()); return false; }
    Pamt pamt;
    if (!pamt.parse(pamtData.data(), pamtData.size())) {
        QS_ERR("[%s] %s/0.pamt parse error: %s", id, e.group.c_str(), pamt.error().c_str());
        return false;
    }
    if (pamt.storedSelfHash() != pamt.computedSelfHash() || pamt.storedSelfHash() != e.pamtHash) {
        QS_ERR("[%s] %s/0.pamt hash stored %08X computed %08X expected %08X -> entry disabled", id, e.group.c_str(),
               pamt.storedSelfHash(), pamt.computedSelfHash(), e.pamtHash);
        return false;
    }
    PamtEntry pe;
    if (!pamt.find(e.path, pe)) { QS_ERR("[%s] %s not found in %s/0.pamt", id, e.path.c_str(), e.group.c_str()); return false; }
    if (pe.pazIndex != e.pazIndex || pe.offset != e.offset || pe.storedSize != e.storedSize || pe.size != e.size ||
        pe.flags != e.flags) {
        QS_ERR("[%s] index record differs: paz %u off %u stored %u size %u flags %02X (manifest paz %u off %llu stored %u size %u flags %02X)",
               id, pe.pazIndex, pe.offset, pe.storedSize, pe.size, pe.flags, e.pazIndex, (unsigned long long)e.offset,
               e.storedSize, e.size, e.flags);
        return false;
    }
    uint64_t pazTableSize = 0;
    for (const PamtPaz& z : pamt.pazTable())
        if (z.index == e.pazIndex) pazTableSize = z.size;
    std::wstring pazName = std::to_wstring(e.pazIndex) + L".paz";
    std::wstring pazPath = joinPath(joinPath(root, widen(e.group)), pazName);
    uint64_t pazActual = 0;
    if (!fileSize(pazPath, pazActual) || pazActual != pazTableSize || pazActual != e.pazSize) {
        QS_ERR("[%s] %s size %llu, pamt says %llu, manifest %llu -> entry disabled", id, narrow(pazPath).c_str(),
               (unsigned long long)pazActual, (unsigned long long)pazTableSize, (unsigned long long)e.pazSize);
        return false;
    }

    // ---- original bytes ----------------------------------------------------------------------
    std::vector<uint8_t> orig;
    if (!readFileRange(pazPath, e.offset, e.storedSize, orig)) { QS_ERR("[%s] cannot read original range", id); return false; }
    uint32_t oh = cdHash(orig.data(), orig.size()), oh0 = hashlittle(orig.data(), orig.size(), 0);
    if (oh != e.origHash || oh0 != e.origHash0) {
        QS_ERR("[%s] original bytes hash %08X/%08X, expected %08X/%08X -> entry disabled", id, oh, oh0, e.origHash,
               e.origHash0);
        return false;
    }

    // ---- replacement bytes -------------------------------------------------------------------
    std::wstring dataPath = joinPath(manifestDir, widen(e.dataFile));
    std::vector<uint8_t> repl;
    if (!readWholeFile(dataPath, repl)) { QS_ERR("[%s] cannot read %s", id, narrow(dataPath).c_str()); return false; }
    if (repl.size() != orig.size()) {
        QS_ERR("[%s] replacement is %zu bytes, original %zu -> entry disabled", id, repl.size(), orig.size());
        return false;
    }
    uint32_t rh = cdHash(repl.data(), repl.size()), rh0 = hashlittle(repl.data(), repl.size(), 0);
    if (rh != e.dataHash || rh0 != e.dataHash0) {
        QS_ERR("[%s] replacement hash %08X/%08X, manifest %08X/%08X -> entry disabled", id, rh, rh0, e.dataHash, e.dataHash0);
        return false;
    }
    if ((e.flags & 0x0F) == 1) {  // per-mip DDS: header (incl. mip stored sizes) must be identical
        if (orig.size() < 128 || std::memcmp(orig.data(), repl.data(), 128) != 0) {
            QS_ERR("[%s] DDS header of the replacement differs from the original -> entry disabled", id);
            return false;
        }
    }
    // Decode both blobs with the strict reference LZ4 decoder: the original proves our format model,
    // the replacement must decode to exactly the entry size. Encrypted entries are not validated here.
    if ((e.flags >> 4) != 0) {
        QS_INFO("[%s] entry is encrypted (crypto %u): replacement must already be encrypted; decode check skipped", id,
                e.flags >> 4);
    } else if ((e.flags & 0x0F) == 1 || (e.flags & 0x0F) == 2) {
        for (int which = 0; which < 2; ++which) {
            const std::vector<uint8_t>& blob = which ? repl : orig;
            std::string err;
            bool good = (e.flags & 0x0F) == 1 ? validatePerMipDds(blob.data(), blob.size(), e.size, &err)
                                               : lz4VerifyStrict(blob.data(), blob.size(), nullptr, e.size, &err);
            if (!good) {
                QS_ERR("[%s] %s data failed strict decode: %s -> entry disabled", id, which ? "replacement" : "original",
                       err.c_str());
                return false;
            }
        }
        QS_INFO("[%s] strict decode OK for original and replacement (%u -> %u bytes)", id, e.storedSize, e.size);
    }

    // ---- DLC shadowing check (informational) -------------------------------------------------
    std::wstring dlcPamt = joinPath(root, L"dlc\\dlc_01\\" + widen(e.group) + L"\\0.pamt");
    std::vector<uint8_t> dlcData;
    if (readWholeFile(dlcPamt, dlcData)) {
        Pamt dp;
        PamtEntry de;
        if (dp.parse(dlcData.data(), dlcData.size()) && dp.find(e.path, de))
            QS_INFO("[%s] WARNING: dlc_01/%s also contains %s (paz %u @%u): the game may read that copy instead", id,
                    e.group.c_str(), e.path.c_str(), de.pazIndex, de.offset);
    }

    out.name = e.path;
    out.offset = e.offset;
    out.original = std::move(orig);
    out.replacement = std::move(repl);
    target.key = normalizePath(e.group + "/" + narrow(pazName));
    target.fullPathLower = lowerW(pazPath);
    size_t diff = 0;
    for (size_t i = 0; i < out.original.size(); ++i) diff += out.original[i] != out.replacement[i];
    QS_INFO("[%s] OK %s -> %s range [%llu, %llu) %u bytes, flags %02X, %zu bytes differ (%s)", id, e.path.c_str(),
            target.key.c_str(), (unsigned long long)out.offset, (unsigned long long)out.end(), e.storedSize, e.flags, diff,
            e.description.c_str());
    return true;
}

}  // namespace

Config loadConfig(const std::wstring& iniPath) {
    Config c;
    std::vector<uint8_t> raw;
    if (!readWholeFile(iniPath, raw)) return c;  // defaults
    Ini ini;
    ini.parse(std::string(raw.begin(), raw.end()));
    c.enabled = ini.getInt("General", "Enabled", 1) != 0;
    c.substitute = ini.getInt("General", "Substitute", 1) != 0;
    c.logLevel = int(ini.getInt("General", "LogLevel", 1));
    c.maxReadLogLines = int(ini.getInt("General", "MaxReadLogLines", 3000));
    c.waitForTargetReads = ini.getInt("General", "WaitForTargetReads", 1) != 0;
    c.summaryIntervalSeconds = int(ini.getInt("General", "SummaryIntervalSeconds", 30));
    c.testMode = ini.getInt("Debug", "TestMode", 0) != 0;
    c.gameRootOverride = widen(ini.get("Debug", "GameRootOverride"));
    c.activePack = ini.get("Packs", "Active");
    if (ini.has("Packs", "Folder") && !ini.get("Packs", "Folder").empty()) c.packFolder = widen(ini.get("Packs", "Folder"));
    c.packCacheMB = int(ini.getInt("Packs", "CacheMB", 64));
    if (c.packCacheMB < 0) c.packCacheMB = 0;
    if (c.packCacheMB > 4096) c.packCacheMB = 4096;
    return c;
}

// ---- season packs --------------------------------------------------------------------------------
namespace {

// Random access reads of a pack with explicit offsets (thread safe, no shared file pointer).
class WinFileSource : public pack::ByteSource {
public:
    explicit WinFileSource(HANDLE h, uint64_t size) : h_(h), size_(size) {}
    ~WinFileSource() override { CloseHandle(h_); }
    bool readAt(uint64_t offset, void* dst, size_t n) override {
        uint8_t* p = static_cast<uint8_t*>(dst);
        while (n > 0) {
            DWORD chunk = n > (1u << 30) ? (1u << 30) : DWORD(n), got = 0;
            OVERLAPPED ov = {};
            ov.Offset = DWORD(offset);
            ov.OffsetHigh = DWORD(offset >> 32);
            if (!ReadFile(h_, p, chunk, &got, &ov) || got != chunk) return false;
            p += chunk;
            offset += chunk;
            n -= chunk;
        }
        return true;
    }
    uint64_t size() const override { return size_; }

private:
    HANDLE h_;
    uint64_t size_;
};

std::wstring groupDir(const std::wstring& root, const std::string& group) {
    std::wstring g = widen(group);
    for (wchar_t& c : g) if (c == L'/') c = L'\\';
    return joinPath(root, g);
}

}  // namespace

std::string gameBuildIdOf(const std::wstring& gameRoot) {
    const std::wstring exe = joinPath(gameRoot, L"bin64\\CrimsonDesert.exe");
    uint64_t size = 0;
    uint32_t ts = 0;
    if (!fileSize(exe, size) || !readPeTimestamp(exe, ts)) return "unreadable";
    return pack::gameBuildId(size, ts);
}

std::wstring resolvePackPath(const std::wstring& packsDir, const std::string& stageOrFile) {
    gen::StageSpec st;
    std::wstring name;
    const bool looksLikeFile = stageOrFile.find_first_of("\\/:") != std::string::npos ||
                               (stageOrFile.size() > 4 && stageOrFile.compare(stageOrFile.size() - 4, 4, ".qsp") == 0);
    if (!looksLikeFile && gen::parseStage(stageOrFile, 16, st)) name = widen(st.fileName());
    else name = widen(stageOrFile);
    if (name.size() > 1 && (name[1] == L':' || (name[0] == L'\\' && name[1] == L'\\'))) return name;  // absolute
    return joinPath(packsDir, name);
}

namespace {

// Stored and computed self hash of a 0.pamt. Several packs are verified against the same pamt files
// (the plugin opens 2-4 packs at once and more as the days pass), so results are cached per path and
// file size: the game files do not change while the game runs.
bool pamtHashes(const std::wstring& path, uint32_t& stored, uint32_t& computed) {
    struct Entry { uint64_t size; uint32_t stored, computed; };
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::map<std::wstring, Entry> cache;
    uint64_t size = 0;
    if (!fileSize(path, size)) return false;
    AcquireSRWLockShared(&lock);
    auto it = cache.find(path);
    const bool hit = it != cache.end() && it->second.size == size;
    if (hit) { stored = it->second.stored; computed = it->second.computed; }
    ReleaseSRWLockShared(&lock);
    if (hit) return true;
    std::vector<uint8_t> data;
    if (!readWholeFile(path, data) || data.size() < 12) return false;
    std::memcpy(&stored, data.data(), 4);
    computed = cdHash(data.data() + 12, data.size() - 12);
    AcquireSRWLockExclusive(&lock);
    cache[path] = Entry{uint64_t(data.size()), stored, computed};
    ReleaseSRWLockExclusive(&lock);
    return true;
}

}  // namespace

std::shared_ptr<pack::PackServer> loadVerifiedPack(const std::wstring& gameRoot, const std::wstring& packPath,
                                                   size_t cacheBytes, std::string& why) {
    try {
        HANDLE h = CreateFileW(packPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (h == INVALID_HANDLE_VALUE) { why = "cannot open " + narrow(packPath); return nullptr; }
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); why = "cannot size " + narrow(packPath); return nullptr; }
        std::shared_ptr<pack::PackServer> srv(new pack::PackServer());
        std::string err;
        if (!srv->open(std::unique_ptr<pack::ByteSource>(new WinFileSource(h, uint64_t(sz.QuadPart))), cacheBytes, &err)) {
            why = "pack rejected: " + err;
            return nullptr;
        }
        const pack::Header& hd = srv->reader().header();
        const std::string build = gameBuildIdOf(gameRoot);
        if (hd.gameBuild != build) {
            why = "pack was generated for game build \"" + hd.gameBuild + "\", installed is \"" + build + "\": regenerate it";
            return nullptr;
        }
        for (const auto& g : hd.pamtHashes) {
            uint32_t stored = 0, computed = 0;
            if (!pamtHashes(joinPath(groupDir(gameRoot, g.first), L"0.pamt"), stored, computed)) {
                why = "cannot read " + g.first + "/0.pamt";
                return nullptr;
            }
            if (stored != computed || stored != g.second) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s/0.pamt hash %08X (computed %08X), pack expects %08X: regenerate the pack",
                              g.first.c_str(), stored, computed, g.second);
                why = buf;
                return nullptr;
            }
        }
        for (const pack::PazInfo& z : srv->reader().paz()) {
            uint64_t actual = 0;
            const std::wstring p = joinPath(groupDir(gameRoot, z.group), std::to_wstring(z.pazIndex) + L".paz");
            if (!fileSize(p, actual) || actual != z.pazSize) {
                why = z.key() + " size " + std::to_string(actual) + ", pack expects " + std::to_string(z.pazSize);
                return nullptr;
            }
        }
        return srv;
    } catch (...) {
        why = "unexpected exception while loading the pack";
        return nullptr;
    }
}

SetupResult runSetup(const Config& cfg, const std::wstring& scriptsDir) {
    SetupResult r;
    const std::wstring exe = modulePath(nullptr);
    // <root>\bin64\CrimsonDesert.exe -> <root>
    r.gameRoot = cfg.testMode && !cfg.gameRootOverride.empty() ? cfg.gameRootOverride : dirName(dirName(exe));
    QS_INFO("game root: %s", narrow(r.gameRoot).c_str());
    QS_INFO("host exe : %s", narrow(exe).c_str());

    // ---- 1. version file ---------------------------------------------------------------------
    std::vector<uint8_t> paver;
    bool paverOk = readWholeFile(joinPath(r.gameRoot, L"meta\\0.paver"), paver);
    if (paverOk) {
        paverOk = false;
        for (const char* hx : builds::kPaverHexes) {
            std::vector<uint8_t> want;
            parseHex(hx, want);
            paverOk |= paver == want;
        }
    }
    std::string paverHex;
    for (uint8_t b : paver) { static const char* hx = "0123456789abcdef"; paverHex += hx[b >> 4]; paverHex += hx[b & 15]; }
    QS_INFO("version check: meta/0.paver %s (%s, expected %s = %s)", paverOk ? "OK" : "MISMATCH",
            paverHex.empty() ? "unreadable" : paverHex.c_str(), kPaver204Hex, kGameVersion);

    // ---- 2. executable -----------------------------------------------------------------------
    uint64_t exeSize = 0;
    uint32_t ts = 0;
    bool exeOk = false;
    if (fileSize(exe, exeSize) && readPeTimestamp(exe, ts))
        exeOk = builds::findKnownExe(exeSize, ts) != nullptr;
    QS_INFO("version check: exe size %llu timestamp %u (expected %llu / %u) -> %s%s", (unsigned long long)exeSize, ts,
            (unsigned long long)kExeSize204, kExeTimestamp204, exeOk ? "OK" : "MISMATCH",
            (!exeOk && cfg.testMode) ? " (ignored: [Debug] TestMode=1)" : "");
    if (cfg.testMode) exeOk = true;

    r.versionOk = paverOk && exeOk;
    if (!r.versionOk) {
        r.versionReport = "game files do not match 2.04.00: substitution disabled";
        QS_ERR("%s", r.versionReport.c_str());
        return r;
    }

    // ---- 3. manifest entries -----------------------------------------------------------------
    const std::wstring manifestDir = joinPath(scriptsDir, L"QuattroStagioni");
    const std::wstring manifestPath = joinPath(manifestDir, L"manifest.ini");
    std::vector<uint8_t> mraw;
    if (!readWholeFile(manifestPath, mraw)) {
        QS_ERR("cannot read %s: nothing to substitute", narrow(manifestPath).c_str());
        return r;
    }
    Ini ini;
    ini.parse(std::string(mraw.begin(), mraw.end()));
    std::vector<std::string> errors;
    Manifest m = parseManifest(ini, errors);
    for (const auto& e : errors) QS_ERR("manifest: %s", e.c_str());
    if (!m.gameVersion.empty() && m.gameVersion != kGameVersion) {
        QS_ERR("manifest is for game version %s, plugin is built for %s: substitution disabled", m.gameVersion.c_str(), kGameVersion);
        return r;
    }
    for (const ManifestEntry& e : m.entries) {
        if (!e.enabled) { QS_INFO("[%s] disabled in manifest", e.id.c_str()); continue; }
        ArchiveTarget t;
        Substitution s;
        if (!loadEntry(e, r.gameRoot, manifestDir, t, s)) continue;
        bool merged = false;
        for (auto& existing : r.targets) {
            if (existing.key == t.key) { existing.subs.push_back(std::move(s)); merged = true; break; }
        }
        if (!merged) { t.subs.push_back(std::move(s)); r.targets.push_back(std::move(t)); }
    }
    r.versionReport = "2.04.00 OK";
    return r;
}

}  // namespace qs
