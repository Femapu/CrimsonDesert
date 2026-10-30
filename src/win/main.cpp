// QuattroStagioni - entry points.
//
// Loaded by Ultimate ASI Loader from bin64\scripts\. Startup sequence (all failures -> that part
// inactive, game untouched):
//   1. read bin64\scripts\QuattroStagioni.ini, open bin64\scripts\QuattroStagioni.log
//   2. version guard + manifest verification (setup.cpp), reading game files only
//   3. install Win32 file I/O hooks (io_hooks.cpp) and open the season packs written by qs_gen
//   4. season engine tick (season_tick.cpp): calendar, stage -> active pack
//   5. D3D12 overlay with the F5 menu (overlay/), read-only runtime probes (probe/)
// Steps 4-5 also run when the version guard fails (no packs then): the menu and the probes only
// read game state, and the probes are what finds the systems of a new game build.
#include <windows.h>

#include "apply/apply.h"
#include "apply/snow_win.h"
#include "io_hooks.h"
#include "lang.h"
#include "log.h"
#include "overlay/overlay.h"
#include "probe/probe.h"
#include "season_tick.h"
#include "setup.h"
#include "winutil.h"

namespace {

const char* const kVersion = "0.3.0 (season engine, packs, F5 menu, probes)";
HMODULE g_self = nullptr;
volatile LONG g_initDone = 0;
bool g_seasonStarted = false, g_overlayStarted = false, g_probeStarted = false, g_applyStarted = false,
     g_snowStarted = false;

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

        qs::initLanguage(scriptsDir);
        qs::SetupResult setup = qs::runSetup(cfg, scriptsDir);
        bool hooked = false, packs = false;
        if (!setup.versionOk) {
            QS_ERR("version guard failed: no file hooks and no packs, game files are served unmodified");
        } else {
            if (setup.targets.empty()) QS_INFO("no verified substitution: file hooks in observe-only mode");
            hooked = qs::io::install(cfg, setup.gameRoot, std::move(setup.targets));
            if (hooked) packs = qs::io::packsStart(cfg, setup.gameRoot, scriptsDir);
        }
        // MinHook is initialised by io::install; overlay and probes use it afterwards.
        g_seasonStarted = qs::seasonStart(scriptsDir, packs);
        qs::menuSetVersionInfo(kVersion);
        qs::menuSetSaveCallback(&qs::seasonSave);
        g_overlayStarted = qs::overlayStart();
        g_probeStarted = qs::probeStart();
        g_applyStarted = qs::applyStart();  // targets.ini; off by default
        g_snowStarted = qs::snowStart(scriptsDir, setup.versionOk);  // [Snow]: winter snow thresholds
        QueryPerformanceCounter(&t1);
        QS_INFO("startup in %.1f ms: file hooks %s, packs %s, season %s, overlay %s, probes %s, apply %s, snow %s",
                double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart), hooked ? "on" : "off",
                packs ? "on" : "off", g_seasonStarted ? "on" : "off", g_overlayStarted ? "on" : "off",
                g_probeStarted ? "on" : "off", g_applyStarted ? "on" : "off", g_snowStarted ? "on" : "off");
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
                if (g_snowStarted) qs::snowStop();        // restores SnowStart/SnowFull
                if (g_applyStarted) qs::applyStop();      // restores originals, before probeStop
                if (g_probeStarted) qs::probeStop();      // before io::uninstall (MH_Uninitialize)
                if (g_overlayStarted) qs::overlayStop();
                if (g_seasonStarted) qs::seasonStop();
                qs::io::packsStop();
                qs::io::uninstall();
                qs::log::close();
            }
            break;
    }
    return TRUE;
}
