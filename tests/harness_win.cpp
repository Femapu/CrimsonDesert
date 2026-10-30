// QuattroStagioni - Windows test harness (run under Windows or wine).
//
// Builds a miniature fake game folder, loads QuattroStagioni.asi exactly like a plugin, then reads
// the fake 0012\3.paz with every I/O style the hooks support and checks byte-exactly that
//   * the target range comes back as the replacement (test.bin), whatever the chunking,
//   * every byte outside the range is untouched,
//   * other archives are untouched.
// Usage: harness_win.exe <data_dir> <asi_path> <work_dir> [wait|nowait|observe|badversion|badorig]
//   observe/badversion/badorig expect the archive bytes to come back UNCHANGED.
//   data_dir: 0012_0.pamt, cd_loading_00.stored.orig, test.bin, manifest.ini
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const uint64_t kOff = 24565408, kPazSize = 955464432;
static std::vector<uint8_t> g_orig, g_repl, g_noise;
static bool g_expectPatched = true;  // false in "observe" mode (Substitute=0)

static std::vector<uint8_t> readAll(const std::wstring& p) {
    std::vector<uint8_t> v;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return v;
    LARGE_INTEGER s;
    GetFileSizeEx(h, &s);
    v.resize(size_t(s.QuadPart));
    DWORD got = 0;
    if (!v.empty()) ReadFile(h, v.data(), DWORD(v.size()), &got, nullptr);
    CloseHandle(h);
    v.resize(got);
    return v;
}

static bool writeAt(const std::wstring& p, uint64_t off, const void* d, size_t n, uint64_t totalSize) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD r;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &r, nullptr);  // best effort
    LARGE_INTEGER pos;
    pos.QuadPart = LONGLONG(totalSize);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    pos.QuadPart = LONGLONG(off);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    DWORD w = 0;
    BOOL ok = WriteFile(h, d, DWORD(n), &w, nullptr);
    CloseHandle(h);
    return ok && w == n;
}

static bool writeFile(const std::wstring& p, const void* d, size_t n) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, d, DWORD(n), &w, nullptr);
    CloseHandle(h);
    return w == n;
}

// Expected content of the fake archive window [kOff - 65536, kOff + size + 65536).
static uint8_t expectedByte(uint64_t pos, bool patched) {
    if (pos >= kOff && pos < kOff + g_orig.size()) return patched ? g_repl[pos - kOff] : g_orig[pos - kOff];
    if (pos >= kOff - g_noise.size() / 2 && pos < kOff) return g_noise[pos - (kOff - g_noise.size() / 2)];
    uint64_t after = kOff + g_orig.size();
    if (pos >= after && pos < after + g_noise.size() / 2) return g_noise[g_noise.size() / 2 + (pos - after)];
    return 0;  // sparse zeros
}

static bool checkWindow(const std::vector<uint8_t>& buf, uint64_t off, bool patched) {
    for (size_t i = 0; i < buf.size(); ++i)
        if (buf[i] != expectedByte(off + i, patched)) {
            std::printf("   mismatch at file offset %llu (buffer +%zu)\n", (unsigned long long)(off + i), i);
            return false;
        }
    return true;
}

static void testSync(const std::wstring& paz, uint64_t start, size_t total, size_t chunk, bool viaA) {
    HANDLE h;
    if (viaA) {
        std::string a(paz.begin(), paz.end());
        h = CreateFileA(a.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    } else {
        h = CreateFileW(paz.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    CHECK(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER p;
    p.QuadPart = LONGLONG(start);
    SetFilePointerEx(h, p, nullptr, FILE_BEGIN);
    std::vector<uint8_t> all(total);
    size_t done = 0;
    while (done < total) {
        DWORD n = DWORD(std::min(chunk, total - done)), got = 0;
        CHECK(ReadFile(h, all.data() + done, n, &got, nullptr) && got == n);
        done += n;
    }
    CloseHandle(h);
    CHECK(checkWindow(all, start, g_expectPatched));
}

static void testOverlappedEvent(const std::wstring& paz, uint64_t start, size_t total, size_t chunk) {
    HANDLE h = CreateFileW(paz.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    std::vector<uint8_t> all(total);
    for (size_t done = 0; done < total;) {
        OVERLAPPED ov = {};
        uint64_t o = start + done;
        ov.Offset = DWORD(o);
        ov.OffsetHigh = DWORD(o >> 32);
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = DWORD(std::min(chunk, total - done)), got = 0;
        BOOL r = ReadFile(h, all.data() + done, n, nullptr, &ov);
        CHECK(r || GetLastError() == ERROR_IO_PENDING);
        CHECK(GetOverlappedResult(h, &ov, &got, TRUE) && got == n);
        CloseHandle(ov.hEvent);
        done += n;
    }
    CloseHandle(h);
    CHECK(checkWindow(all, start, g_expectPatched));
}

static void testIocp(const std::wstring& paz, uint64_t start, size_t total, size_t chunk, bool ex) {
    HANDLE h = CreateFileW(paz.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    HANDLE port = CreateIoCompletionPort(h, nullptr, 0x1234, 0);
    CHECK(port != nullptr);
    std::vector<uint8_t> all(total);
    size_t count = (total + chunk - 1) / chunk;
    std::vector<OVERLAPPED> ovs(count);
    for (size_t i = 0; i < count; ++i) {  // issue all reads first (out-of-order completion allowed)
        uint64_t o = start + i * chunk;
        ovs[i] = OVERLAPPED{};
        ovs[i].Offset = DWORD(o);
        ovs[i].OffsetHigh = DWORD(o >> 32);
        DWORD n = DWORD(std::min(chunk, total - i * chunk));
        BOOL r = ReadFile(h, all.data() + i * chunk, n, nullptr, &ovs[i]);
        CHECK(r || GetLastError() == ERROR_IO_PENDING);
    }
    size_t completed = 0;
    while (completed < count) {
        if (ex) {
            OVERLAPPED_ENTRY e[8];
            ULONG got = 0;
            CHECK(GetQueuedCompletionStatusEx(port, e, 8, &got, 5000, FALSE));
            completed += got;
        } else {
            DWORD n;
            ULONG_PTR key;
            LPOVERLAPPED o = nullptr;
            CHECK(GetQueuedCompletionStatus(port, &n, &key, &o, 5000) && key == 0x1234);
            ++completed;
        }
    }
    CloseHandle(h);
    CloseHandle(port);
    CHECK(checkWindow(all, start, g_expectPatched));
}

static volatile LONG g_apcs = 0;
static VOID CALLBACK apc(DWORD err, DWORD n, LPOVERLAPPED) { if (err == 0) InterlockedIncrement(&g_apcs); }

static void testReadFileEx(const std::wstring& paz, uint64_t start, size_t total, size_t chunk) {
    HANDLE h = CreateFileW(paz.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    std::vector<uint8_t> all(total);
    size_t count = (total + chunk - 1) / chunk;
    std::vector<OVERLAPPED> ovs(count);
    g_apcs = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t o = start + i * chunk;
        ovs[i] = OVERLAPPED{};
        ovs[i].Offset = DWORD(o);
        ovs[i].OffsetHigh = DWORD(o >> 32);
        CHECK(ReadFileEx(h, all.data() + i * chunk, DWORD(std::min(chunk, total - i * chunk)), &ovs[i], apc));
    }
    for (int i = 0; i < 200 && g_apcs < LONG(count); ++i) SleepEx(20, TRUE);
    CHECK(g_apcs == LONG(count));
    CloseHandle(h);
    CHECK(checkWindow(all, start, g_expectPatched));
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { std::printf("usage: harness_win <data_dir> <asi> <work_dir>\n"); return 2; }
    std::wstring data = argv[1], asi = argv[2], root = argv[3];
    std::wstring mode = argc > 4 ? argv[4] : L"wait";
    g_expectPatched = mode == L"wait" || mode == L"nowait";
    std::printf("mode: %ls\n", mode.c_str());
    g_orig = readAll(data + L"\\cd_loading_00.stored.orig");
    g_repl = readAll(data + L"\\test.bin");
    std::vector<uint8_t> pamt = readAll(data + L"\\0012_0.pamt");
    std::vector<uint8_t> manifest = readAll(data + L"\\manifest.ini");
    if (g_orig.size() != 30119 || g_repl.size() != 30119 || pamt.empty() || manifest.empty()) {
        std::printf("missing data files\n");
        return 2;
    }
    if (mode == L"badorig") g_orig[15000] ^= 0xFF;  // archive content differs from the manifest hash
    g_noise.resize(131072);
    for (size_t i = 0; i < g_noise.size(); ++i) g_noise[i] = uint8_t((i * 2654435761u) >> 13);

    // ---- fake game folder ----------------------------------------------------------------------
    CreateDirectoryW(root.c_str(), nullptr);
    for (const wchar_t* d : {L"\\bin64", L"\\bin64\\scripts", L"\\bin64\\scripts\\QuattroStagioni", L"\\meta", L"\\0012"})
        CreateDirectoryW((root + d).c_str(), nullptr);
    const uint8_t paver[] = {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
                             'd', 'l', 'c', '_', '0', '1', 0x00, 0x00, 0x00, 0x00, 0x47, 0x39, 0x6a, 0x31};
    uint8_t pv[sizeof(paver)];
    std::memcpy(pv, paver, sizeof(paver));
    if (mode == L"badversion") pv[2] = 0x05;  // pretend 2.05.00
    writeFile(root + L"\\meta\\0.paver", pv, sizeof(pv));
    writeFile(root + L"\\0012\\0.pamt", pamt.data(), pamt.size());
    const std::wstring paz = root + L"\\0012\\3.paz";
    DeleteFileW(paz.c_str());
    CHECK(writeAt(paz, kOff - g_noise.size() / 2, g_noise.data(), g_noise.size() / 2, kPazSize));
    CHECK(writeAt(paz, kOff, g_orig.data(), g_orig.size(), kPazSize));
    CHECK(writeAt(paz, kOff + g_orig.size(), g_noise.data() + g_noise.size() / 2, g_noise.size() / 2, kPazSize));
    const std::wstring other = root + L"\\0012\\6.paz";
    writeFile(other, g_orig.data(), g_orig.size());  // same bytes in a non-target archive: must stay
    const std::wstring scripts = root + L"\\bin64\\scripts";
    writeFile(scripts + L"\\QuattroStagioni\\manifest.ini", manifest.data(), manifest.size());
    writeFile(scripts + L"\\QuattroStagioni\\test.bin", g_repl.data(), g_repl.size());
    std::string ini = std::string("[General]\r\nEnabled=1\r\nSubstitute=") + (mode == L"observe" ? "0" : "1") +
                      "\r\nWaitForTargetReads=" + (mode == L"nowait" ? "0" : "1") +
                      "\r\nLogLevel=2\r\nMaxReadLogLines=200\r\n[Debug]\r\nTestMode=1\r\nGameRootOverride=";
    for (wchar_t c : root) ini.push_back(char(c));
    ini += "\r\n";
    writeFile(scripts + L"\\QuattroStagioni.ini", ini.data(), ini.size());
    std::wstring asiDst = scripts + L"\\QuattroStagioni.asi";
    CHECK(CopyFileW(asi.c_str(), asiDst.c_str(), FALSE));

    // Unhooked reference read: the archive really holds the ORIGINAL bytes.
    {
        HANDLE h = CreateFileW(paz.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        std::vector<uint8_t> b(g_orig.size() + 200);
        LARGE_INTEGER p;
        p.QuadPart = LONGLONG(kOff - 100);
        SetFilePointerEx(h, p, nullptr, FILE_BEGIN);
        DWORD got;
        ReadFile(h, b.data(), DWORD(b.size()), &got, nullptr);
        CloseHandle(h);
        CHECK(checkWindow(b, kOff - 100, false));
    }

    // ---- load the plugin ---------------------------------------------------------------------
    HMODULE m = LoadLibraryW(asiDst.c_str());
    CHECK(m != nullptr);
    if (!m) { std::printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto init = (void (*)())GetProcAddress(m, "InitializeASI");
    CHECK(init != nullptr);
    if (init) init();  // no-op if DllMain already initialised

    const uint64_t start = kOff - 40000;
    const size_t total = g_orig.size() + 80000;
    std::printf("sync reads...\n");
    testSync(paz, start, total, 4096, false);
    testSync(paz, start, total, 1000, false);
    testSync(paz, start, total, 7777, true);       // CreateFileA
    testSync(paz, kOff, g_orig.size(), g_orig.size(), false);  // exactly the entry
    testSync(paz, kOff + 5, 10, 3, false);         // tiny reads inside the entry
    std::printf("overlapped + event...\n");
    testOverlappedEvent(paz, start, total, 65536);
    testOverlappedEvent(paz, start, total, 3001);
    std::printf("IOCP...\n");
    testIocp(paz, start, total, 16384, false);
    testIocp(paz, start, total, 5000, true);
    std::printf("ReadFileEx...\n");
    testReadFileEx(paz, start, total, 12345);
    std::printf("relative path...\n");
    {
        wchar_t prev[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, prev);
        SetCurrentDirectoryW((root + L"\\bin64").c_str());
        testSync(L"..\\0012\\3.paz", start, total, 65536, false);
        SetCurrentDirectoryW(prev);
    }
    std::printf("non-target archive...\n");
    {
        std::vector<uint8_t> b = readAll(other);
        CHECK(b == g_orig);
    }

    FreeLibrary(m);
    // Show the plugin log.
    std::vector<uint8_t> log = readAll(scripts + L"\\QuattroStagioni.log");
    std::fwrite(log.data(), 1, log.size(), stdout);
    std::printf("\nHARNESS: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
