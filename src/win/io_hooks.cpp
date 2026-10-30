// QuattroStagioni - Win32 file I/O hooks.
//
// What is hooked (KernelBase exports, kernel32 as fallback; nothing inside CrimsonDesert.exe):
//   CreateFileW / CreateFileA / CreateFile2   -> remember handles of *.paz files under the game root
//   ReadFile / ReadFileEx / ReadFileScatter   -> see every read of a tracked handle (offset, length)
//   GetOverlappedResult(Ex)                   -> completion of overlapped reads (event / handle wait)
//   GetQueuedCompletionStatus(Ex)             -> completion of overlapped reads through an IOCP
//   CloseHandle                               -> forget handles
//   CreateFileMappingW/A                      -> detect (and log) memory-mapped archive access
//
// Substitution happens only when a read that overlaps a verified target range has COMPLETED, by
// overwriting the overlapping bytes inside the game's own buffer (see core/range_patch). Sizes,
// offsets and the files on disk are never changed. Every hook calls the original function first or
// unconditionally, preserves GetLastError(), and falls back to pure pass-through on any problem.
//
// Season packs: several packs (one per visual stage) are open at once; an immutable per-texture
// snapshot (core/pack/multi.h, published through an atomic shared_ptr slot) says which pack serves
// each texture. Every completed read of a tracked .paz takes a copy of the snapshot pointer (never a
// long lock), finds the overlapping textures by binary search and applies the record of the serving
// pack; original bytes are verified per 4 KiB chunk against hashes stored in the pack, and the edges
// of unaligned reads are completed from a private, unhooked handle on the same paz. Payloads come
// from the pack files on demand through bounded caches. The season tick (setSeasonDay) opens and
// closes packs and republishes the snapshot when the day crosses a texture's stage boundary; a read in
// flight during a swap is served by the snapshot current when it completes.
#include "io_hooks.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

#include "../core/pack/multi.h"
#include "MinHook.h"
#include "log.h"
#include "winutil.h"

namespace qs {
namespace io {
namespace {

#ifndef STATUS_PENDING
#define STATUS_PENDING ((DWORD)0x00000103L)
#endif

// ---------------------------------------------------------------------------------------------
// Types of the hooked functions
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using ReadFileEx_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
using ReadFileScatter_t = BOOL(WINAPI*)(HANDLE, FILE_SEGMENT_ELEMENT*, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResult_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using GetOverlappedResultEx_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
using GQCS_t = BOOL(WINAPI*)(HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD);
using GQCSEx_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED_ENTRY, ULONG, PULONG, DWORD, BOOL);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
using CreateFileMappingW_t = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
using CreateFileMappingA_t = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);

CreateFileW_t oCreateFileW = nullptr;
CreateFileA_t oCreateFileA = nullptr;
CreateFile2_t oCreateFile2 = nullptr;
ReadFile_t oReadFile = nullptr;
ReadFileEx_t oReadFileEx = nullptr;
ReadFileScatter_t oReadFileScatter = nullptr;
GetOverlappedResult_t oGetOverlappedResult = nullptr;
GetOverlappedResultEx_t oGetOverlappedResultEx = nullptr;
GQCS_t oGQCS = nullptr;
GQCSEx_t oGQCSEx = nullptr;
CloseHandle_t oCloseHandle = nullptr;
CreateFileMappingW_t oCreateFileMappingW = nullptr;
CreateFileMappingA_t oCreateFileMappingA = nullptr;

// ---------------------------------------------------------------------------------------------
// State
struct Archive {
    std::string key;                // "0012/3.paz" relative to the game root (or full path)
    std::wstring path;              // canonical full path (for the private original-bytes handle)
    std::vector<Substitution> subs; // phase-2 manifest substitutions (empty for most archives)
    volatile LONG64 opens = 0, reads = 0, bytes = 0, asyncReads = 0, targetReads = 0;
    volatile LONG64 patched = 0, already = 0, mismatch = 0, mappings = 0, scatter = 0;
    volatile LONG mappingLogged = 0, scatterLogged = 0;
    // season pack
    volatile LONG64 slotCache = -1;   // (texture index id << 32) | (slot + 1), -1 = unresolved
    volatile LONG64 packReads = 0, packPatched = 0, packAlready = 0, packRefused = 0;
    HANDLE priv = INVALID_HANDLE_VALUE;  // private handle for original-chunk reads (opened lazily)
    SRWLOCK privLock = SRWLOCK_INIT;
};

struct Pending {
    Archive* archive = nullptr;
    uint64_t offset = 0;
    uint8_t* buffer = nullptr;
    DWORD length = 0;
    LPOVERLAPPED_COMPLETION_ROUTINE userRoutine = nullptr;  // ReadFileEx only
    bool isEx = false;
    bool done = false;
};

Config g_cfg;
std::wstring g_rootLower;  // canonical game root, lower case, with trailing '\'

SRWLOCK g_hLock = SRWLOCK_INIT;  // protects g_handles and g_archives
std::unordered_map<HANDLE, Archive*> g_handles;
std::map<std::string, Archive*> g_archives;
std::map<std::wstring, Archive*> g_targetByPath;  // canonical lower path -> target archive
volatile LONG g_handleCount = 0;

SRWLOCK g_pLock = SRWLOCK_INIT;  // protects g_pending
std::unordered_map<OVERLAPPED*, Pending> g_pending;
volatile LONG g_pendingCount = 0;

SRWLOCK g_patchLock = SRWLOCK_INIT;  // serialises buffer patching (phase-2 manifest entries)

// Season pack state. Hooks only read g_serve. Everything in g_drv belongs to the pack driver
// (setSeasonDay / setActivePack / packsStop), serialised by g_packLoadLock, which hooks never take.
pack::SnapshotSlot g_serve;
SRWLOCK g_packLoadLock = SRWLOCK_INIT;
std::wstring g_packsDir, g_packRoot;
size_t g_packCacheBytes = size_t(64) << 20;
volatile LONG g_packsStarted = 0;
volatile LONG g_packHitLog = 0, g_packErrLog = 0;
const LONG kPackHitLogLines = 20, kPackErrLogLines = 50;
volatile LONG g_readLogLines = 0;
volatile LONG g_installed = 0;

// API call counters (only calls that concern tracked .paz handles, except where noted)
volatile LONG64 c_openW = 0, c_openA = 0, c_open2 = 0, c_read = 0, c_readEx = 0, c_scatter = 0, c_gor = 0,
                c_gorEx = 0, c_gqcs = 0, c_gqcsEx = 0, c_mapping = 0, c_waits = 0;
volatile LONG64 c_lastSummaryReads = -1;

LARGE_INTEGER g_qpf;
inline double msSince(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
}

// Canonical path of an open handle, lower case, without the \\?\ prefix.
std::wstring canonicalPath(HANDLE h) {
    wchar_t buf[1024];
    DWORD n = GetFinalPathNameByHandleW(h, buf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= 1024) return {};
    std::wstring p(buf, n);
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    return lowerW(p);
}

inline bool endsWithPaz(const wchar_t* name) {
    if (!name) return false;
    size_t n = wcslen(name);
    return n >= 4 && name[n - 4] == L'.' && (name[n - 3] | 0x20) == L'p' && (name[n - 2] | 0x20) == L'a' &&
           (name[n - 1] | 0x20) == L'z';
}
inline bool endsWithPazA(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    return n >= 4 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'p' && (name[n - 2] | 0x20) == 'a' &&
           (name[n - 1] | 0x20) == 'z';
}

inline Archive* lookupHandle(HANDLE h) {
    if (g_handleCount == 0 || h == INVALID_HANDLE_VALUE || h == nullptr) return nullptr;
    AcquireSRWLockShared(&g_hLock);
    auto it = g_handles.find(h);
    Archive* a = it == g_handles.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_hLock);
    return a;
}

// Called after a successful open of a *.paz file.
void registerPaz(HANDLE h, const char* api, DWORD flags) {
    std::wstring path = canonicalPath(h);
    if (path.empty()) return;
    std::string key;
    if (!g_rootLower.empty() && path.rfind(g_rootLower, 0) == 0) {
        key = narrow(path.substr(g_rootLower.size()));
        for (char& c : key) if (c == '\\') c = '/';
    } else {
        key = narrow(path);
    }
    Archive* a = nullptr;
    bool fresh = false;
    AcquireSRWLockExclusive(&g_hLock);
    auto known = g_handles.find(h);
    if (known != g_handles.end()) {  // already registered by a nested Create* hook
        ReleaseSRWLockExclusive(&g_hLock);
        return;
    }
    auto t = g_targetByPath.find(path);
    if (t != g_targetByPath.end()) {
        a = t->second;
    } else {
        auto it = g_archives.find(key);
        if (it != g_archives.end()) a = it->second;
        else {
            a = new (std::nothrow) Archive();
            if (a) { a->key = key; g_archives[key] = a; fresh = true; }
        }
    }
    if (a) {
        if (a->path.empty()) a->path = path;
        g_handles[h] = a;
        InterlockedIncrement(&g_handleCount);
        InterlockedIncrement64(&a->opens);
    }
    ReleaseSRWLockExclusive(&g_hLock);
    if (!a) return;
    if (!a->subs.empty())
        QS_INFO("open TARGET archive %s handle %p via %s flags %08lX%s%s", a->key.c_str(), h, api, flags,
                (flags & FILE_FLAG_OVERLAPPED) ? " [overlapped]" : " [synchronous]",
                (flags & FILE_FLAG_NO_BUFFERING) ? " [no-buffering]" : "");
    else if (fresh || log::enabled(LogLevel::Debug))
        QS_INFO("open %s handle %p via %s flags %08lX%s%s", a->key.c_str(), h, api, flags,
                (flags & FILE_FLAG_OVERLAPPED) ? " [overlapped]" : "", (flags & FILE_FLAG_NO_BUFFERING) ? " [no-buffering]" : "");
}

// Slot of this archive in the snapshot's texture index, cached per archive; -1 if no open pack has it.
int packSlot(Archive* a, const pack::ServeSnapshot* s) {
    if (!s || !s->index) return -1;
    const LONG64 id = LONG64(s->index->id());
    LONG64 c = a->slotCache;
    if (c != -1 && (c >> 32) == id) return int(c & 0xFFFFFFFF) - 1;
    int slot = s->slotFor(a->key);
    InterlockedExchange64(&a->slotCache, (id << 32) | LONG64(uint32_t(slot + 1)));
    return slot;
}

bool packTouches(Archive* a, uint64_t off, uint64_t len) {
    if (!g_packsStarted) return false;
    std::shared_ptr<const pack::ServeSnapshot> s = g_serve.get();
    int slot = packSlot(a, s.get());
    return slot >= 0 && s->touches(slot, off, len);
}

bool touchesTarget(Archive* a, uint64_t off, uint64_t len) {
    for (const Substitution& s : a->subs)
        if (overlaps(s, off, len)) return true;
    return packTouches(a, off, len);
}

// Original bytes of a paz through a private handle opened with the UNHOOKED CreateFileW (so it is
// never tracked) and read with the unhooked ReadFile at explicit offsets.
class DiskOriginal : public pack::OriginalReader {
public:
    explicit DiskOriginal(Archive* a) : a_(a) {}
    bool readOriginal(uint64_t off, uint8_t* dst, size_t n) override {
        HANDLE h = handle();
        if (h == INVALID_HANDLE_VALUE || n > 0x7FFFFFFF) return false;
        OVERLAPPED ov = {};
        ov.Offset = DWORD(off);
        ov.OffsetHigh = DWORD(off >> 32);
        DWORD got = 0;
        BOOL r = oReadFile(h, dst, DWORD(n), &got, &ov);
        if (!r && GetLastError() == ERROR_IO_PENDING) r = oGetOverlappedResult(h, &ov, &got, TRUE);
        return r && got == n;
    }

private:
    HANDLE handle() {
        AcquireSRWLockShared(&a_->privLock);
        HANDLE h = a_->priv;
        ReleaseSRWLockShared(&a_->privLock);
        if (h != INVALID_HANDLE_VALUE || a_->path.empty() || !oCreateFileW) return h;
        AcquireSRWLockExclusive(&a_->privLock);
        if (a_->priv == INVALID_HANDLE_VALUE)
            a_->priv = oCreateFileW(a_->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        h = a_->priv;
        ReleaseSRWLockExclusive(&a_->privLock);
        return h;
    }
    Archive* a_;
};

void logRead(const char* api, const Archive* a, HANDLE h, uint64_t off, DWORD len, bool async) {
    if (!log::enabled(LogLevel::Debug)) return;
    LONG n = InterlockedIncrement(&g_readLogLines);
    if (n <= g_cfg.maxReadLogLines)
        QS_DBG("read %s [%llu, +%lu) via %s%s handle %p", a->key.c_str(), (unsigned long long)off, len, api,
               async ? " (async)" : "", h);
    else if (n == g_cfg.maxReadLogLines + 1)
        QS_DBG("per-read log limit (%d lines) reached; only summaries and target hits from now on", g_cfg.maxReadLogLines);
}

// Applies all substitutions of `a` to a completed read.
void patchCompleted(Archive* a, uint64_t off, uint8_t* buf, size_t got, const char* via) {
    if (!a || !buf || got == 0) return;
    for (const Substitution& s : a->subs) {
        if (!overlaps(s, off, got)) continue;
        if (!g_cfg.substitute) {
            QS_INFO("HIT (observe only) %s: read [%llu, +%zu) of %s via %s - not modified (Substitute=0)",
                    s.name.c_str(), (unsigned long long)off, got, a->key.c_str(), via);
            continue;
        }
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        AcquireSRWLockExclusive(&g_patchLock);
        PatchInfo pi = applyToRead(s, off, buf, got);
        ReleaseSRWLockExclusive(&g_patchLock);
        double ms = msSince(t0);
        switch (pi.result) {
            case PatchResult::Patched: InterlockedIncrement64(&a->patched); break;
            case PatchResult::AlreadyPatched:
            case PatchResult::Identical: InterlockedIncrement64(&a->already); break;
            case PatchResult::Mismatch: InterlockedIncrement64(&a->mismatch); break;
            default: break;
        }
        if (pi.result == PatchResult::Mismatch)
            QS_ERR("HIT %s: read [%llu, +%zu) via %s overlap [%llu, %llu) holds unexpected bytes -> left untouched",
                   s.name.c_str(), (unsigned long long)off, got, via, (unsigned long long)pi.overlapBegin,
                   (unsigned long long)pi.overlapEnd);
        else
            QS_INFO("HIT %s: read [%llu, +%zu) of %s via %s overlap [%llu, %llu) (%llu of %zu entry bytes) -> %s in %.3f ms",
                    s.name.c_str(), (unsigned long long)off, got, a->key.c_str(), via,
                    (unsigned long long)pi.overlapBegin, (unsigned long long)pi.overlapEnd,
                    (unsigned long long)(pi.overlapEnd - pi.overlapBegin), s.original.size(), toString(pi.result), ms);
    }

    // ---- season pack ----
    if (!g_packsStarted) return;
    std::shared_ptr<const pack::ServeSnapshot> p = g_serve.get();
    int slot = packSlot(a, p.get());
    if (slot < 0 || !p->touches(slot, off, got)) return;
    InterlockedIncrement64(&a->packReads);
    if (!g_cfg.substitute) {
        if (InterlockedIncrement(&g_packHitLog) <= kPackHitLogLines)
            QS_INFO("PACK HIT (observe only) %s [%llu, +%zu) via %s - not modified (Substitute=0)", a->key.c_str(),
                    (unsigned long long)off, got, via);
        return;
    }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    DiskOriginal disk(a);
    pack::ApplyReport rep = p->apply(slot, off, buf, got, &disk);
    const double ms = msSince(t0);
    InterlockedExchangeAdd64(&a->packPatched, rep.patched);
    InterlockedExchangeAdd64(&a->packAlready, rep.already);
    const uint32_t refused = rep.mismatch + rep.unverifiable + rep.payloadErrors;
    InterlockedExchangeAdd64(&a->packRefused, refused);
    const char* name = rep.first ? rep.first->path.c_str() : "?";
    if (refused) {
        if (InterlockedIncrement(&g_packErrLog) <= kPackErrLogLines)
            QS_ERR("PACK %s: read [%llu, +%zu) of %s via %s: %u record(s) left untouched (mismatch %u, unverifiable %u, "
                   "payload errors %u)", name, (unsigned long long)off, got, a->key.c_str(), via, refused, rep.mismatch,
                   rep.unverifiable, rep.payloadErrors);
    } else if (InterlockedIncrement(&g_packHitLog) <= kPackHitLogLines) {
        QS_INFO("PACK HIT %s: read [%llu, +%zu) of %s via %s: %u record(s) patched, %u already, %llu bytes, %u edge "
                "chunk(s) verified from disk, %.3f ms", name, (unsigned long long)off, got, a->key.c_str(), via,
                rep.patched, rep.already, (unsigned long long)rep.bytesPatched, rep.partialChunks, ms);
    } else {
        QS_DBG("PACK HIT %s: read [%llu, +%zu) via %s: patched %u already %u, %.3f ms", name, (unsigned long long)off, got,
               via, rep.patched, rep.already, ms);
    }
}

void addPending(OVERLAPPED* ov, const Pending& p) {
    AcquireSRWLockExclusive(&g_pLock);
    auto r = g_pending.insert_or_assign(ov, p);
    if (r.second) InterlockedIncrement(&g_pendingCount);
    ReleaseSRWLockExclusive(&g_pLock);
}

bool takePending(OVERLAPPED* ov, Pending& out, bool keepEx) {
    if (g_pendingCount == 0 || !ov) return false;
    AcquireSRWLockExclusive(&g_pLock);
    auto it = g_pending.find(ov);
    bool found = it != g_pending.end();
    if (found) {
        out = it->second;
        if (keepEx && it->second.isEx) {
            it->second.done = true;  // APC still needs the user routine
        } else {
            g_pending.erase(it);
            InterlockedDecrement(&g_pendingCount);
        }
    }
    ReleaseSRWLockExclusive(&g_pLock);
    return found;
}

// Completion seen by a wait/dequeue API: patch the buffer if this overlapped belongs to a target read.
void completeOverlapped(OVERLAPPED* ov, DWORD bytes, bool success, const char* via) {
    if (g_pendingCount == 0 || !ov) return;
    Pending p;
    if (!takePending(ov, p, true)) return;
    if (success && !p.done) {
        DWORD got = bytes <= p.length ? bytes : p.length;
        patchCompleted(p.archive, p.offset, p.buffer, got, via);
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks
HANDLE WINAPI hkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                            HANDLE tmpl) {
    HANDLE h = oCreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE && endsWithPaz(name)) {
        DWORD e = GetLastError();
        InterlockedIncrement64(&c_openW);
        try { registerPaz(h, "CreateFileW", flags); } catch (...) {}
        SetLastError(e);
    }
    return h;
}

HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                            HANDLE tmpl) {
    HANDLE h = oCreateFileA(name, access, share, sa, disp, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE && endsWithPazA(name)) {
        DWORD e = GetLastError();
        InterlockedIncrement64(&c_openA);
        try { registerPaz(h, "CreateFileA", flags); } catch (...) {}
        SetLastError(e);
    }
    return h;
}

HANDLE WINAPI hkCreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS ex) {
    HANDLE h = oCreateFile2(name, access, share, disp, ex);
    if (h != INVALID_HANDLE_VALUE && endsWithPaz(name)) {
        DWORD e = GetLastError();
        InterlockedIncrement64(&c_open2);
        DWORD flags = ex ? (ex->dwFileFlags | ex->dwFileAttributes) : 0;
        try { registerPaz(h, "CreateFile2", flags); } catch (...) {}
        SetLastError(e);
    }
    return h;
}

BOOL WINAPI hkReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD read, LPOVERLAPPED ov) {
    Archive* a = lookupHandle(h);
    if (!a) return oReadFile(h, buf, len, read, ov);

    uint64_t off = 0;
    bool offKnown = true;
    if (ov) {
        off = (uint64_t(ov->OffsetHigh) << 32) | ov->Offset;
    } else {
        LARGE_INTEGER z, cur;
        z.QuadPart = 0;
        offKnown = SetFilePointerEx(h, z, &cur, FILE_CURRENT) != FALSE;
        off = offKnown ? uint64_t(cur.QuadPart) : 0;
    }
    InterlockedIncrement64(&c_read);
    InterlockedIncrement64(&a->reads);
    InterlockedExchangeAdd64(&a->bytes, len);
    logRead("ReadFile", a, h, off, len, ov != nullptr);

    const bool target = offKnown && touchesTarget(a, off, len);
    if (target) {
        InterlockedIncrement64(&a->targetReads);
        if (!a->subs.empty())
            QS_INFO("target read issued: %s [%llu, +%lu) via ReadFile%s", a->key.c_str(), (unsigned long long)off, len,
                    ov ? " (overlapped)" : "");
        else
            QS_DBG("pack read issued: %s [%llu, +%lu) via ReadFile%s", a->key.c_str(), (unsigned long long)off, len,
                   ov ? " (overlapped)" : "");
        if (ov) {  // register before the call so a fast completion on another thread is not missed
            Pending p;
            p.archive = a;
            p.offset = off;
            p.buffer = static_cast<uint8_t*>(buf);
            p.length = len;
            addPending(ov, p);
        }
    }

    BOOL r = oReadFile(h, buf, len, read, ov);
    if (!target) return r;

    DWORD err = GetLastError();
    try {
        if (r) {  // completed synchronously
            DWORD got = read ? *read : (ov ? DWORD(ov->InternalHigh) : 0);
            if (ov) { Pending dummy; takePending(ov, dummy, false); }
            patchCompleted(a, off, static_cast<uint8_t*>(buf), got, ov ? "ReadFile/overlapped-sync" : "ReadFile/sync");
        } else if (ov && err == ERROR_IO_PENDING) {
            InterlockedIncrement64(&a->asyncReads);
            if (g_cfg.waitForTargetReads) {
                // Wait (without consuming the game's event or completion packet) until the kernel has
                // filled the buffer, then patch before returning to the caller.
                InterlockedIncrement64(&c_waits);
                LARGE_INTEGER t0;
                QueryPerformanceCounter(&t0);
                volatile ULONG_PTR* status = &ov->Internal;
                DWORD spins = 0;
                while (*status == STATUS_PENDING && msSince(t0) < 5000.0) {
                    if (++spins < 64) SwitchToThread();
                    else Sleep(1);
                }
                ULONG_PTR st = *status;
                if (st == 0 /*STATUS_SUCCESS*/) {
                    completeOverlapped(ov, DWORD(ov->InternalHigh), true, "ReadFile/wait");
                } else if (st == STATUS_PENDING) {
                    QS_INFO("target read still pending after 5 s; will patch on completion notification");
                } else {
                    Pending dummy;
                    takePending(ov, dummy, false);
                    QS_INFO("target read failed with NTSTATUS %08lX", (unsigned long)st);
                }
            }
        } else if (ov) {
            Pending dummy;
            takePending(ov, dummy, false);
        }
    } catch (...) {
    }
    SetLastError(err);
    return r;
}

VOID CALLBACK exCompletion(DWORD err, DWORD bytes, LPOVERLAPPED ov) {
    Pending p;
    bool have = takePending(ov, p, false);
    if (have && err == 0 && !p.done) {
        DWORD got = bytes <= p.length ? bytes : p.length;
        patchCompleted(p.archive, p.offset, p.buffer, got, "ReadFileEx/APC");
    }
    if (have && p.userRoutine) p.userRoutine(err, bytes, ov);
}

BOOL WINAPI hkReadFileEx(HANDLE h, LPVOID buf, DWORD len, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cb) {
    Archive* a = lookupHandle(h);
    if (!a || !ov) return oReadFileEx(h, buf, len, ov, cb);
    uint64_t off = (uint64_t(ov->OffsetHigh) << 32) | ov->Offset;
    InterlockedIncrement64(&c_readEx);
    InterlockedIncrement64(&a->reads);
    InterlockedExchangeAdd64(&a->bytes, len);
    logRead("ReadFileEx", a, h, off, len, true);
    if (!touchesTarget(a, off, len)) return oReadFileEx(h, buf, len, ov, cb);

    InterlockedIncrement64(&a->targetReads);
    if (!a->subs.empty())
        QS_INFO("target read issued: %s [%llu, +%lu) via ReadFileEx", a->key.c_str(), (unsigned long long)off, len);
    else
        QS_DBG("pack read issued: %s [%llu, +%lu) via ReadFileEx", a->key.c_str(), (unsigned long long)off, len);
    Pending p;
    p.archive = a;
    p.offset = off;
    p.buffer = static_cast<uint8_t*>(buf);
    p.length = len;
    p.userRoutine = cb;
    p.isEx = true;
    addPending(ov, p);
    BOOL r = oReadFileEx(h, buf, len, ov, exCompletion);
    if (!r) {
        DWORD e = GetLastError();
        Pending dummy;
        takePending(ov, dummy, false);
        SetLastError(e);
    }
    return r;
}

BOOL WINAPI hkReadFileScatter(HANDLE h, FILE_SEGMENT_ELEMENT* seg, DWORD len, LPDWORD reserved, LPOVERLAPPED ov) {
    Archive* a = lookupHandle(h);
    if (a && ov) {
        uint64_t off = (uint64_t(ov->OffsetHigh) << 32) | ov->Offset;
        InterlockedIncrement64(&c_scatter);
        InterlockedIncrement64(&a->scatter);
        InterlockedIncrement64(&a->reads);
        InterlockedExchangeAdd64(&a->bytes, len);
        logRead("ReadFileScatter", a, h, off, len, true);
        if (touchesTarget(a, off, len) && InterlockedExchange(&a->scatterLogged, 1) == 0)
            QS_ERR("target range of %s read via ReadFileScatter [%llu, +%lu): not supported, left untouched (logged once)",
                   a->key.c_str(), (unsigned long long)off, len);
    }
    return oReadFileScatter(h, seg, len, reserved, ov);
}

BOOL WINAPI hkGetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD n, BOOL wait) {
    BOOL r = oGetOverlappedResult(h, ov, n, wait);
    if (g_pendingCount > 0 && ov) {
        DWORD e = GetLastError();
        InterlockedIncrement64(&c_gor);
        try { completeOverlapped(ov, n ? *n : DWORD(ov->InternalHigh), r != FALSE, "GetOverlappedResult"); } catch (...) {}
        SetLastError(e);
    }
    return r;
}

BOOL WINAPI hkGetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov, LPDWORD n, DWORD ms, BOOL alertable) {
    BOOL r = oGetOverlappedResultEx(h, ov, n, ms, alertable);
    if (g_pendingCount > 0 && ov) {
        DWORD e = GetLastError();
        InterlockedIncrement64(&c_gorEx);
        // FALSE with WAIT_TIMEOUT / WAIT_IO_COMPLETION / ERROR_IO_INCOMPLETE means "not finished yet"
        if (r || (e != WAIT_TIMEOUT && e != WAIT_IO_COMPLETION && e != ERROR_IO_INCOMPLETE)) {
            try { completeOverlapped(ov, n ? *n : DWORD(ov->InternalHigh), r != FALSE, "GetOverlappedResultEx"); } catch (...) {}
        }
        SetLastError(e);
    }
    return r;
}

BOOL WINAPI hkGQCS(HANDLE port, LPDWORD n, PULONG_PTR key, LPOVERLAPPED* ov, DWORD ms) {
    BOOL r = oGQCS(port, n, key, ov, ms);
    if (g_pendingCount > 0 && ov && *ov) {
        DWORD e = GetLastError();
        InterlockedIncrement64(&c_gqcs);
        try { completeOverlapped(*ov, n ? *n : 0, r != FALSE, "GetQueuedCompletionStatus"); } catch (...) {}
        SetLastError(e);
    }
    return r;
}

BOOL WINAPI hkGQCSEx(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG count, PULONG removed, DWORD ms, BOOL alertable) {
    BOOL r = oGQCSEx(port, entries, count, removed, ms, alertable);
    if (r && g_pendingCount > 0 && entries && removed) {
        DWORD e = GetLastError();
        InterlockedIncrement64(&c_gqcsEx);
        try {
            for (ULONG i = 0; i < *removed && i < count; ++i) {
                OVERLAPPED* o = entries[i].lpOverlapped;
                if (!o) continue;
                bool ok = o->Internal == 0;  // per-entry status lives in the OVERLAPPED
                completeOverlapped(o, entries[i].dwNumberOfBytesTransferred, ok, "GetQueuedCompletionStatusEx");
            }
        } catch (...) {
        }
        SetLastError(e);
    }
    return r;
}

BOOL WINAPI hkCloseHandle(HANDLE h) {
    if (g_handleCount > 0 && h != nullptr && h != INVALID_HANDLE_VALUE) {
        Archive* a = nullptr;
        if (lookupHandle(h)) {  // cheap shared lookup first: CloseHandle is called very often
            AcquireSRWLockExclusive(&g_hLock);
            auto it = g_handles.find(h);
            if (it != g_handles.end()) {
                a = it->second;
                g_handles.erase(it);
                InterlockedDecrement(&g_handleCount);
            }
            ReleaseSRWLockExclusive(&g_hLock);
        }
        if (a && !a->subs.empty())
            QS_INFO("close TARGET archive %s handle %p (reads %lld, target reads %lld, patched %lld)", a->key.c_str(), h,
                    (long long)a->reads, (long long)a->targetReads, (long long)a->patched);
    }
    return oCloseHandle(h);
}

void noteMapping(HANDLE file, const char* api) {
    Archive* a = lookupHandle(file);
    if (!a) return;
    InterlockedIncrement64(&c_mapping);
    InterlockedIncrement64(&a->mappings);
    if (InterlockedExchange(&a->mappingLogged, 1) == 0)
        QS_ERR("%s on archive %s: memory-mapped access is NOT substituted in phase 2%s", api, a->key.c_str(),
               a->subs.empty() ? "" : " (this is a TARGET archive)");
}

HANDLE WINAPI hkCreateFileMappingW(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name) {
    HANDLE r = oCreateFileMappingW(f, sa, prot, hi, lo, name);
    if (g_handleCount > 0) { DWORD e = GetLastError(); noteMapping(f, "CreateFileMappingW"); SetLastError(e); }
    return r;
}

HANDLE WINAPI hkCreateFileMappingA(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) {
    HANDLE r = oCreateFileMappingA(f, sa, prot, hi, lo, name);
    if (g_handleCount > 0) { DWORD e = GetLastError(); noteMapping(f, "CreateFileMappingA"); SetLastError(e); }
    return r;
}

// ---------------------------------------------------------------------------------------------
// Installation
struct HookSpec {
    const char* name;
    void* detour;
    void** original;
    bool required;
    void* target = nullptr;
    const char* module = nullptr;
};

DWORD WINAPI summaryThread(LPVOID) {
    for (;;) {
        Sleep(DWORD(g_cfg.summaryIntervalSeconds > 0 ? g_cfg.summaryIntervalSeconds : 30) * 1000u);
        if (!g_installed) return 0;
        logSummary("periodic");
    }
}

}  // namespace

bool install(const Config& cfg, const std::wstring& gameRoot, std::vector<ArchiveTarget>&& targets) {
    g_cfg = cfg;
    QueryPerformanceFrequency(&g_qpf);

    // Canonical root (resolves short names / links the same way GetFinalPathNameByHandle does).
    HANDLE dir = CreateFileW(gameRoot.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir != INVALID_HANDLE_VALUE) {
        g_rootLower = canonicalPath(dir);
        CloseHandle(dir);
    }
    if (g_rootLower.empty()) g_rootLower = lowerW(gameRoot);
    if (!g_rootLower.empty() && g_rootLower.back() != L'\\') g_rootLower.push_back(L'\\');

    for (ArchiveTarget& t : targets) {
        std::wstring canon;
        HANDLE f = CreateFileW(t.fullPathLower.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            canon = canonicalPath(f);
            CloseHandle(f);
        }
        if (canon.empty()) canon = t.fullPathLower;
        Archive* a = new Archive();
        a->key = t.key;
        a->path = canon;
        a->subs = std::move(t.subs);
        g_archives[a->key] = a;
        g_targetByPath[canon] = a;
        QS_INFO("target archive %s = %s (%zu substitution%s)", a->key.c_str(), narrow(canon).c_str(), a->subs.size(),
                a->subs.size() == 1 ? "" : "s");
    }

    if (MH_Initialize() != MH_OK) {
        QS_ERR("MinHook initialisation failed: running without hooks");
        return false;
    }
    HookSpec specs[] = {
        {"CreateFileW", (void*)hkCreateFileW, (void**)&oCreateFileW, true},
        {"CreateFileA", (void*)hkCreateFileA, (void**)&oCreateFileA, false},
        {"CreateFile2", (void*)hkCreateFile2, (void**)&oCreateFile2, false},
        {"ReadFile", (void*)hkReadFile, (void**)&oReadFile, true},
        {"ReadFileEx", (void*)hkReadFileEx, (void**)&oReadFileEx, false},
        {"ReadFileScatter", (void*)hkReadFileScatter, (void**)&oReadFileScatter, false},
        {"GetOverlappedResult", (void*)hkGetOverlappedResult, (void**)&oGetOverlappedResult, true},
        {"GetOverlappedResultEx", (void*)hkGetOverlappedResultEx, (void**)&oGetOverlappedResultEx, false},
        {"GetQueuedCompletionStatus", (void*)hkGQCS, (void**)&oGQCS, true},
        {"GetQueuedCompletionStatusEx", (void*)hkGQCSEx, (void**)&oGQCSEx, false},
        {"CloseHandle", (void*)hkCloseHandle, (void**)&oCloseHandle, true},
        {"CreateFileMappingW", (void*)hkCreateFileMappingW, (void**)&oCreateFileMappingW, false},
        {"CreateFileMappingA", (void*)hkCreateFileMappingA, (void**)&oCreateFileMappingA, false},
    };
    const wchar_t* modules[] = {L"kernelbase.dll", L"kernel32.dll"};
    bool ok = true;
    for (HookSpec& s : specs) {
        for (const wchar_t* m : modules) {
            HMODULE h = GetModuleHandleW(m);
            if (!h) continue;
            void* p = (void*)GetProcAddress(h, s.name);
            if (!p) continue;
            if (MH_CreateHook(p, s.detour, s.original) == MH_OK) {
                s.target = p;
                s.module = (m == modules[0]) ? "KernelBase" : "kernel32";
                break;
            }
        }
        if (!s.target) {
            if (s.required) { QS_ERR("cannot hook required API %s", s.name); ok = false; }
            else QS_INFO("optional API %s not hooked (not found)", s.name);
        }
    }
    if (ok && MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        QS_ERR("MH_EnableHook failed");
        ok = false;
    }
    if (!ok) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        QS_ERR("hooks removed: plugin is inactive (pass-through)");
        return false;
    }
    for (const HookSpec& s : specs)
        if (s.target) QS_INFO("hooked %s!%s at %p", s.module, s.name, s.target);
    InterlockedExchange(&g_installed, 1);
    HANDLE th = CreateThread(nullptr, 0, summaryThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    return true;
}

void logSummary(const char* reason) {
    if (!g_installed) return;
    LONG64 totalReads = 0;
    std::vector<Archive*> list;
    AcquireSRWLockShared(&g_hLock);
    for (auto& kv : g_archives) { list.push_back(kv.second); totalReads += kv.second->reads; }
    LONG open = g_handleCount;
    ReleaseSRWLockShared(&g_hLock);
    if (std::string(reason) == "periodic" && totalReads == c_lastSummaryReads) return;  // nothing new
    c_lastSummaryReads = totalReads;
    QS_INFO("---- summary (%s): %zu archives seen, %ld paz handles open, pending target reads %ld ----", reason, list.size(),
            open, g_pendingCount);
    QS_INFO("api calls on paz: CreateFileW %lld, CreateFileA %lld, CreateFile2 %lld, ReadFile %lld, ReadFileEx %lld, "
            "ReadFileScatter %lld, mappings %lld; completions checked: GOR %lld, GORex %lld, GQCS %lld, GQCSex %lld, waits %lld",
            (long long)c_openW, (long long)c_openA, (long long)c_open2, (long long)c_read, (long long)c_readEx,
            (long long)c_scatter, (long long)c_mapping, (long long)c_gor, (long long)c_gorEx, (long long)c_gqcs,
            (long long)c_gqcsEx, (long long)c_waits);
    {
        std::shared_ptr<const pack::ServeSnapshot> snap = g_serve.get();
        if (snap && snap->index && !snap->index->packs().empty()) {
            const auto& packs = snap->index->packs();
            QS_INFO("packs (%s): %zu open, %zu textures indexed, %zu served (%zu by a fallback stage), %zu whose stage "
                    "has no pack%s%s", snap->forced ? ("override " + snap->forcedStage).c_str() : "per texture",
                    packs.size(), snap->index->textures().size(), snap->servedTextures, snap->fallbackTextures,
                    snap->missingTextures, snap->missingStages.empty() ? "" : "; missing stages: ",
                    [&] { std::string m; for (const auto& id : snap->missingStages) m += (m.empty() ? "" : " ") + id; return m; }().c_str());
            for (size_t i = 0; i < packs.size(); ++i) {
                const pack::LoadedPack& lp = *packs[i];
                pack::ServeStats s = lp.server->stats();
                QS_INFO("pack %s: serves %u textures (%u as fallback); patched %llu (fallback %llu), already %llu, LEFT "
                        "UNTOUCHED: mismatch %llu unverifiable %llu payload errors %llu (disabled records %llu); edge "
                        "chunks read from disk %llu; %.1f MB patched; cache %llu hits / %llu misses, %.1f MB in use",
                        lp.stageId.c_str(), i < snap->packAssigned.size() ? snap->packAssigned[i] : 0,
                        i < snap->packFallback.size() ? snap->packFallback[i] : 0, (unsigned long long)lp.patched.load(),
                        (unsigned long long)lp.fallbackPatched.load(), (unsigned long long)lp.already.load(),
                        (unsigned long long)s.mismatch, (unsigned long long)s.unverifiable,
                        (unsigned long long)s.payloadErrors, (unsigned long long)s.disabledRecords,
                        (unsigned long long)s.partialChunks, double(s.bytesPatched) / 1048576.0,
                        (unsigned long long)s.cacheHits, (unsigned long long)s.cacheMisses, double(s.cacheBytes) / 1048576.0);
            }
        } else if (g_packsStarted) {
            QS_INFO("pack: none open");
        }
    }
    for (Archive* a : list) {
        if (a->packReads)
            QS_INFO("  %-28s pack reads %lld, records patched %lld, already %lld, refused %lld", a->key.c_str(),
                    (long long)a->packReads, (long long)a->packPatched, (long long)a->packAlready, (long long)a->packRefused);
        if (a->reads == 0 && a->subs.empty() && !log::enabled(LogLevel::Debug)) continue;
        QS_INFO("  %-28s opens %lld reads %lld (%.1f MB, async %lld, scatter %lld) mappings %lld%s", a->key.c_str(),
                (long long)a->opens, (long long)a->reads, double(a->bytes) / 1048576.0, (long long)a->asyncReads,
                (long long)a->scatter, (long long)a->mappings, a->subs.empty() ? "" : "  <TARGET>");
        if (!a->subs.empty())
            QS_INFO("  %-28s target reads %lld, patched %lld, already-patched %lld, mismatch %lld", "",
                    (long long)a->targetReads, (long long)a->patched, (long long)a->already, (long long)a->mismatch);
    }
    log::flush();
}

void uninstall() {
    if (!InterlockedExchange(&g_installed, 0)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

// ---------------------------------------------------------------------------------------------
// Season packs: the driver (setSeasonDay / setActivePack). Runs in the tick or menu thread, under
// g_packLoadLock; publishes immutable snapshots through g_serve for the hooks.
namespace {

const double kLookaheadDays = 0.25;           // packs are opened this much game time before they are needed
const ULONGLONG kRescanMs = 30000;            // folder listing refresh
const ULONGLONG kRetryMs = 300000;            // retry a refused pack file after 5 minutes
const ULONGLONG kPlanMs = 60000;              // refresh of the menu plan
const int kDefaultPlanDays = 7;

struct Driver {
    std::map<std::string, std::shared_ptr<pack::LoadedPack>> open;  // season mode: stage id -> pack
    std::shared_ptr<const pack::TextureIndex> index;                // index of `open`
    std::shared_ptr<const pack::ServeSnapshot> snap;                // last season snapshot published
    std::map<std::string, std::string> refused;                     // stage id -> reason
    std::map<std::string, ULONGLONG> refusedAt;
    bool forced = false;
    std::string forcedStage;
    std::shared_ptr<pack::LoadedPack> forcedPack;
    bool haveDay = false;
    double day = 0;
    season::SeasonConfig cfg;
    std::string cfgKey;
    std::vector<std::string> present;                               // stage ids with a file in the folder
    std::map<std::string, uint64_t> presentBytes;                   // file sizes
    ULONGLONG presentAt = 0;
    uint32_t regionsSeen = 0;                                       // regions of every texture seen so far
    pack::StagePlan plan;
    ULONGLONG planAt = 0;
    uint64_t swaps = 0;
    std::string lastError;
};
Driver g_drv;

// What packsStatus() copies (published by the driver, read by the menu).
SRWLOCK g_statusLock = SRWLOCK_INIT;
PacksStatus g_status;

void rescanFolder(Driver& d) {
    d.present.clear();
    d.presentBytes.clear();
    d.presentAt = GetTickCount64();
    if (g_packsDir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(joinPath(g_packsDir, L"*.qsp").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string id = pack::stageIdFromFileName(narrow(fd.cFileName));
        if (id.empty()) continue;
        d.present.push_back(id);
        d.presentBytes[id] = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// Present stage files that are not in their refusal back-off.
std::vector<std::string> usablePresent(Driver& d) {
    std::vector<std::string> v;
    const ULONGLONG now = GetTickCount64();
    for (const std::string& id : d.present) {
        auto it = d.refusedAt.find(id);
        if (it != d.refusedAt.end() && now - it->second < kRetryMs) continue;
        v.push_back(id);
    }
    return v;
}

// Opens and verifies one pack. Returns nullptr (and remembers why) on failure.
std::shared_ptr<pack::LoadedPack> openPack(Driver& d, const std::string& stageOrFile, std::string& why) {
    const std::wstring path = resolvePackPath(g_packsDir, stageOrFile);
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    std::shared_ptr<pack::PackServer> srv = loadVerifiedPack(g_packRoot, path, g_packCacheBytes, why);
    if (!srv) {
        QS_ERR("packs: %s refused: %s", narrow(path).c_str(), why.c_str());
        d.lastError = narrow(path) + ": " + why;
        return nullptr;
    }
    const pack::PackReader& r = srv->reader();
    std::shared_ptr<pack::LoadedPack> lp(new pack::LoadedPack());
    lp->stageId = r.header().stageId;
    lp->server = srv;
    const size_t slash = path.find_last_of(L"\\/");
    lp->file = narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
    QS_INFO("packs: %s verified in %.1f ms: stage %s, format v%u, %zu textures in %zu archives, %.1f MB payload, "
            "index %.2f MB, generator \"%s\"", narrow(path).c_str(), msSince(t0), lp->stageId.c_str(),
            r.header().formatVersion, r.records().size(), r.paz().size(), double(r.payloadSize()) / 1048576.0,
            double(r.indexBytes()) / 1048576.0, r.header().generator.c_str());
    for (const pack::Record& rec : r.records()) d.regionsSeen |= 1u << (rec.hasMeta ? rec.region : 0);
    return lp;
}

void splitCache(const std::vector<std::shared_ptr<pack::LoadedPack>>& packs) {
    if (packs.empty()) return;
    const size_t each = std::max<size_t>(g_packCacheBytes / packs.size(), size_t(4) << 20);
    for (const auto& p : packs) p->server->setCacheLimit(each);
}

std::string joinIds(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s.empty() ? "-" : s;
}

void refreshPlan(Driver& d, int days) {
    if (!d.haveDay) return;
    uint64_t avg = 0;
    for (const auto& kv : d.presentBytes) avg += kv.second;
    if (!d.presentBytes.empty()) avg /= d.presentBytes.size();
    const uint32_t mask = d.regionsSeen ? (d.regionsSeen & pack::enabledRegionMask(d.cfg)) : pack::enabledRegionMask(d.cfg);
    d.plan = pack::planStages(d.cfg, d.day, double(days), mask, d.present, [&](const std::string& id) -> uint64_t {
        auto it = d.presentBytes.find(id);
        return it != d.presentBytes.end() ? it->second : avg;
    });
    d.planAt = GetTickCount64();
}

// Copies the driver state into g_status (counters are added live by packsStatus()).
void publishStatus(Driver& d) {
    PacksStatus st;
    st.started = g_packsStarted != 0;
    st.forced = d.forced;
    st.forcedStage = d.forcedStage;
    st.seasonDriven = !d.forced && d.haveDay;
    st.day = d.day;
    st.available = d.present;
    for (const auto& kv : d.refused) st.refused.push_back(kv.first + ": " + kv.second);
    st.planDays = kDefaultPlanDays;
    st.plan = d.plan;
    st.cacheLimitBytes = g_packCacheBytes;
    st.snapshotSwaps = d.swaps;
    st.lastError = d.lastError;
    AcquireSRWLockExclusive(&g_statusLock);
    g_status = std::move(st);
    ReleaseSRWLockExclusive(&g_statusLock);
}

void publish(Driver& d, std::shared_ptr<const pack::ServeSnapshot> s) {
    g_serve.set(std::move(s));
    d.swaps++;
}

// Season mode update (lock held). `force`: recompute even if nothing seems to have changed.
void seasonUpdate(Driver& d, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (d.presentAt == 0 || now - d.presentAt > kRescanMs) rescanFolder(d);
    const std::vector<std::string> usable = usablePresent(d);
    const uint32_t mask = d.regionsSeen ? (d.regionsSeen & pack::enabledRegionMask(d.cfg)) : pack::enabledRegionMask(d.cfg);
    const pack::StagePlan soon = pack::planStages(d.cfg, d.day, kLookaheadDays, mask, usable);
    const std::vector<std::string> wanted = pack::packsToOpen(d.cfg, soon, usable);
    std::vector<std::string> openIds;
    for (const auto& kv : d.open) openIds.push_back(kv.first);
    std::vector<std::string> wantedSorted = wanted;
    std::sort(wantedSorted.begin(), wantedSorted.end());
    bool indexChanged = false;
    if (wantedSorted != openIds || !d.index) {
        std::map<std::string, std::shared_ptr<pack::LoadedPack>> next;
        std::vector<std::string> opened, closed;
        for (const std::string& id : wanted) {
            auto it = d.open.find(id);
            if (it != d.open.end()) { next[id] = it->second; continue; }
            std::string why;
            std::shared_ptr<pack::LoadedPack> lp = openPack(d, id, why);
            if (lp && lp->stageId != id) {
                why = "the file is named for stage " + id + " but contains stage " + lp->stageId;
                QS_ERR("packs: %s", why.c_str());
                lp.reset();
            }
            if (!lp) {
                d.refused[id] = why;
                d.refusedAt[id] = GetTickCount64();
                continue;
            }
            d.refused.erase(id);
            d.refusedAt.erase(id);
            next[id] = lp;
            opened.push_back(id);
        }
        for (const auto& kv : d.open)
            if (!next.count(kv.first)) closed.push_back(kv.first);
        d.open = std::move(next);
        std::vector<std::shared_ptr<pack::LoadedPack>> packs;
        for (const std::string& id : wanted)
            if (d.open.count(id)) packs.push_back(d.open[id]);
        d.index = pack::TextureIndex::build(packs);
        splitCache(packs);
        indexChanged = true;
        std::vector<std::string> openNow;
        for (const auto& p : packs) openNow.push_back(p->stageId);
        QS_INFO("packs: day %.2f (%s): open [%s] (opened [%s], closed [%s]), %zu textures indexed%s",
                d.day, d.cfg.dayToLabel(d.day).c_str(), joinIds(openNow).c_str(), joinIds(opened).c_str(),
                joinIds(closed).c_str(), d.index->textures().size(),
                d.index->conflicts() ? (", " + std::to_string(d.index->conflicts()) + " conflicting records ignored").c_str() : "");
        refreshPlan(d, kDefaultPlanDays);
    }
    // A config change resets d.snap before this call (setSeasonDay), so it also lands here.
    if (force || indexChanged || !d.snap || d.day >= d.snap->validUntil || d.day < d.snap->day) {
        const pack::ServeSnapshot* prev = (indexChanged || force || !d.snap) ? nullptr : d.snap.get();
        std::vector<std::string> oldMissing = d.snap ? d.snap->missingStages : std::vector<std::string>();
        d.snap = pack::assignStages(d.index, d.cfg, d.day, prev);
        publish(d, d.snap);
        if (indexChanged || d.snap->missingStages != oldMissing)
            QS_INFO("packs: %zu textures served (%zu by a fallback stage), %zu whose stage has no pack; missing stages "
                    "[%s]", d.snap->servedTextures, d.snap->fallbackTextures, d.snap->missingTextures,
                    joinIds(d.snap->missingStages).c_str());
        else
            QS_DBG("packs: day %.4f, %zu textures re-evaluated, next change at day %.4f", d.day,
                   d.snap->updatedTextures, d.snap->validUntil);
    }
    if (now - d.planAt > kPlanMs) refreshPlan(d, kDefaultPlanDays);
}

void closeSeasonPacks(Driver& d) {
    d.open.clear();
    d.index.reset();
    d.snap.reset();
}

}  // namespace

bool packsStart(const Config& cfg, const std::wstring& gameRoot, const std::wstring& scriptsDir) {
    if (!g_installed) {
        QS_ERR("packs: hooks are not installed, season packs disabled");
        return false;
    }
    const std::wstring& f = cfg.packFolder;
    const bool absolute = f.size() > 1 && (f[1] == L':' || (f[0] == L'\\' && f[1] == L'\\'));
    g_packsDir = absolute ? f : joinPath(scriptsDir, f);
    g_packRoot = gameRoot;
    g_packCacheBytes = size_t(cfg.packCacheMB) << 20;
    InterlockedExchange(&g_packsStarted, 1);
    QS_INFO("packs: folder %s, cache %d MB, game build %s", narrow(g_packsDir).c_str(), cfg.packCacheMB,
            gameBuildIdOf(gameRoot).c_str());
    if (!cfg.activePack.empty()) {
        std::string why;
        if (!setActivePack(cfg.activePack, &why))
            QS_ERR("packs: initial pack %s not activated: %s", cfg.activePack.c_str(), why.c_str());
    } else {
        QS_INFO("packs: automatic per-texture stages (no [Packs] Active= override)");
        AcquireSRWLockExclusive(&g_packLoadLock);
        rescanFolder(g_drv);
        publishStatus(g_drv);
        ReleaseSRWLockExclusive(&g_packLoadLock);
    }
    return true;
}

void setSeasonDay(double day, const season::SeasonConfig& cfg) {
    if (!g_packsStarted || !std::isfinite(day)) return;
    AcquireSRWLockExclusive(&g_packLoadLock);
    try {
        Driver& d = g_drv;
        const std::string key = pack::stageConfigKey(cfg);
        const bool cfgChanged = key != d.cfgKey;
        d.cfg = cfg;
        d.cfgKey = key;
        d.day = day;
        const bool first = !d.haveDay;
        d.haveDay = true;
        if (!d.forced) {
            if (cfgChanged) d.snap.reset();  // full re-evaluation with the new calendar
            seasonUpdate(d, first || cfgChanged);
            if (cfgChanged) refreshPlan(d, kDefaultPlanDays);
        }
        publishStatus(d);
    } catch (...) {
        QS_ERR("packs: unexpected exception in setSeasonDay");
    }
    ReleaseSRWLockExclusive(&g_packLoadLock);
}

bool setActivePack(const std::string& stageOrFile, std::string* whyOut) {
    std::string why;
    bool ok = false;
    if (!g_packsStarted) {
        if (whyOut) *whyOut = "packs not started";
        return false;
    }
    std::string lower;
    for (char c : stageOrFile) lower += char(tolower(static_cast<unsigned char>(c)));
    const bool none = lower.empty() || lower == "none";
    const bool vanilla = lower == "u" || lower == "vanilla";
    AcquireSRWLockExclusive(&g_packLoadLock);
    try {
        Driver& d = g_drv;
        rescanFolder(d);  // a user action: pick up packs generated meanwhile
        if (none) {
            d.forced = false;
            d.forcedStage.clear();
            d.forcedPack.reset();
            if (d.haveDay) {
                seasonUpdate(d, true);
                QS_INFO("packs: override ended, back to per-texture stages");
            } else {
                publish(d, nullptr);
                QS_INFO("packs: override ended, no season day yet: vanilla textures");
            }
            ok = true;
        } else if (vanilla) {
            d.forced = true;
            d.forcedStage.clear();
            d.forcedPack.reset();
            closeSeasonPacks(d);
            publish(d, nullptr);
            QS_INFO("packs: override: vanilla textures");
            ok = true;
        } else {
            std::shared_ptr<pack::LoadedPack> lp = openPack(d, stageOrFile, why);
            if (lp) {
                d.forced = true;
                d.forcedStage = lp->stageId;
                d.forcedPack = lp;
                closeSeasonPacks(d);
                lp->server->setCacheLimit(g_packCacheBytes);
                std::shared_ptr<const pack::TextureIndex> idx = pack::TextureIndex::build({lp});
                publish(d, pack::assignForced(idx, lp->stageId));
                InterlockedExchange(&g_packHitLog, 0);
                QS_INFO("packs: override: stage %s for every texture (subsequent reads)", lp->stageId.c_str());
                ok = true;
            } else {
                QS_ERR("packs: override %s not applied (the previous state stays)", stageOrFile.c_str());
            }
        }
        publishStatus(d);
    } catch (...) {
        why = "unexpected exception";
    }
    ReleaseSRWLockExclusive(&g_packLoadLock);
    if (whyOut) *whyOut = why;
    return ok;
}

std::shared_ptr<const pack::ServeSnapshot> serveSnapshot() { return g_serve.get(); }

std::string activePackStage() {
    AcquireSRWLockShared(&g_statusLock);
    std::string s = g_status.forced ? g_status.forcedStage : std::string();
    ReleaseSRWLockShared(&g_statusLock);
    return s;
}

std::vector<std::string> openPackStages() {
    std::vector<std::string> v;
    std::shared_ptr<const pack::ServeSnapshot> s = g_serve.get();
    if (s && s->index)
        for (const auto& p : s->index->packs()) v.push_back(p->stageId);
    return v;
}

PacksStatus packsStatus() {
    PacksStatus st;
    AcquireSRWLockShared(&g_statusLock);
    st = g_status;
    ReleaseSRWLockShared(&g_statusLock);
    st.started = g_packsStarted != 0;
    std::shared_ptr<const pack::ServeSnapshot> s = g_serve.get();
    if (!s || !s->index) return st;
    const auto& packs = s->index->packs();
    st.texturesIndexed = uint32_t(s->index->textures().size());
    st.texturesServed = uint32_t(s->servedTextures);
    st.texturesFallback = uint32_t(s->fallbackTextures);
    st.texturesMissing = uint32_t(s->missingTextures);
    st.missingNow = s->missingStages;
    st.indexBytes = s->index->bytes();
    for (size_t i = 0; i < packs.size(); ++i) {
        const pack::LoadedPack& lp = *packs[i];
        const pack::PackReader& r = lp.server->reader();
        PackStatus p;
        p.stageId = lp.stageId;
        p.file = lp.file;
        p.formatVersion = r.header().formatVersion;
        p.textures = uint32_t(r.records().size());
        p.assigned = i < s->packAssigned.size() ? s->packAssigned[i] : 0;
        p.assignedFallback = i < s->packFallback.size() ? s->packFallback[i] : 0;
        p.patched = lp.patched.load();
        p.fallbackPatched = lp.fallbackPatched.load();
        p.already = lp.already.load();
        p.mismatch = lp.mismatch.load();
        p.refused = lp.refused.load();
        p.payloadBytes = r.payloadSize();
        p.cacheBytes = lp.server->stats().cacheBytes;
        st.cacheBytes += p.cacheBytes;
        st.open.push_back(std::move(p));
    }
    return st;
}

pack::StagePlan planStages(int days) {
    pack::StagePlan p;
    if (days < 0) days = 0;
    AcquireSRWLockExclusive(&g_packLoadLock);
    try {
        if (g_drv.presentAt == 0) rescanFolder(g_drv);
        refreshPlan(g_drv, days);
        p = g_drv.plan;
        refreshPlan(g_drv, kDefaultPlanDays);  // keep the status window as it was
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&g_packLoadLock);
    return p;
}

std::vector<std::string> availablePacks() {
    std::vector<std::string> v;
    if (g_packsDir.empty()) return v;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(joinPath(g_packsDir, L"*.qsp").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back(narrow(fd.cFileName));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return v;
}

void packsStop() {
    if (!InterlockedExchange(&g_packsStarted, 0)) return;
    AcquireSRWLockExclusive(&g_packLoadLock);
    g_drv.forcedPack.reset();
    g_drv.forced = false;
    closeSeasonPacks(g_drv);
    g_serve.set(nullptr);
    publishStatus(g_drv);
    ReleaseSRWLockExclusive(&g_packLoadLock);
    AcquireSRWLockShared(&g_hLock);
    for (auto& kv : g_archives) {
        Archive* a = kv.second;
        AcquireSRWLockExclusive(&a->privLock);
        if (a->priv != INVALID_HANDLE_VALUE) {
            if (oCloseHandle) oCloseHandle(a->priv);
            else CloseHandle(a->priv);
            a->priv = INVALID_HANDLE_VALUE;
        }
        ReleaseSRWLockExclusive(&a->privLock);
    }
    ReleaseSRWLockShared(&g_hLock);
}

}  // namespace io
}  // namespace qs
