// QuattroStagioni (phase 2) - entry points.
//
// Loaded by Ultimate ASI Loader from bin64\scripts\. Startup sequence (all failures -> inactive,
// game untouched):
//   1. read bin64\scripts\QuattroStagioni.ini, open bin64\scripts\QuattroStagioni.log
//   2. version guard + manifest verification (setup.cpp), reading game files only
//   3. install Win32 file I/O hooks (io_hooks.cpp)
// Later phases (seasonal foliage, live GPU swaps, calendar, F5 menu) plug in after step 3.
#include <windows.h>

#include "io_hooks.h"
#include "log.h"
#include "setup.h"
#include "winutil.h"

namespace {

const char* const kVersion = "0.2.0 (phase 2: foundation, single texture substitution)";
HMODULE g_self = nullptr;
volatile LONG g_initDone = 0;

void initialize() {
    if (InterlockedExchange(&g_initDone, 1) != 0) return;  // run once
    try {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);

        const std::wstring scriptsDir = qs::dirName(qs::modulePath(g_self));
        const qs::Config cfg = qs::loadConfig(qs::joinPath(scriptsDir, L"QuattroStagioni.ini"));
        qs::log::setLevel(cfg.logLevel);
        qs::log::open(qs::joinPath(scriptsDir, L"QuattroStagioni.log"));
        QS_INFO("QuattroStagioni %s starting, pid %lu, plugin %s", kVersion, GetCurrentProcessId(),
                qs::narrow(qs::modulePath(g_self)).c_str());
        QS_INFO("config: Enabled=%d Substitute=%d LogLevel=%d MaxReadLogLines=%d WaitForTargetReads=%d%s", cfg.enabled,
                cfg.substitute, cfg.logLevel, cfg.maxReadLogLines, cfg.waitForTargetReads,
                cfg.testMode ? " [Debug] TestMode=1" : "");
        if (!cfg.enabled) {
            QS_INFO("disabled by config (Enabled=0): no hooks installed");
            return;
        }

        qs::SetupResult setup = qs::runSetup(cfg, scriptsDir);
        if (!setup.versionOk) {
            QS_ERR("version guard failed: no hooks installed, game runs unmodified");
            return;
        }
        if (setup.targets.empty()) QS_INFO("no verified substitution: hooks installed in observe-only mode");

        bool hooked = qs::io::install(cfg, setup.gameRoot, std::move(setup.targets));
        QueryPerformanceCounter(&t1);
        QS_INFO("startup %s in %.1f ms", hooked ? "complete, hooks active" : "FAILED, plugin inactive",
                double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
        qs::log::flush();
    } catch (...) {
        QS_ERR("unexpected exception during startup: plugin inactive");
    }
}

// True if the process contains Ultimate ASI Loader (it calls InitializeASI for us).
bool ultimateAsiLoaderPresent() {
    const wchar_t* names[] = {L"winmm.dll", L"dinput8.dll", L"version.dll", L"dsound.dll", L"winhttp.dll", L"xinput1_3.dll"};
    for (const wchar_t* n : names) {
        HMODULE m = GetModuleHandleW(n);
        if (m && GetProcAddress(m, "IsUltimateASILoader")) return true;
    }
    return false;
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI() { initialize(); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_self = inst;
            DisableThreadLibraryCalls(inst);
            // Another loader that only calls LoadLibrary: initialise here (kept loader-lock safe:
            // file reads and MinHook only, no threads waited on, no DLL loads).
            if (!ultimateAsiLoaderPresent()) initialize();
            break;
        case DLL_PROCESS_DETACH:
            // Process exit (reserved != NULL): other threads are already gone and may have died holding
            // one of our locks, so do nothing at all (the periodic summaries are already in the log).
            if (g_initDone && !reserved) {  // FreeLibrary: final summary, remove hooks, close the log
                qs::io::logSummary("unload");
                QS_INFO("QuattroStagioni unloading");
                qs::io::uninstall();
                qs::log::close();
            }
            break;
    }
    return TRUE;
}
