// QuattroStagioni - seasonal day length in the game (see daylength_win.h).
#include "daylength_win.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/apply/weather.h"
#include "../log.h"
#include "../qs_state.h"
#include "../winutil.h"
#include "feature_status.h"
#include "weather_win.h"

namespace qs {
namespace daylengthimpl {
namespace {

const char* const kKey = "daylength";
constexpr int kStuckTicks = 600;        // about 10 s at 60 fps: the game does not recompose the field
constexpr int kWriteFailLimit = 50;
constexpr float kMinShiftHours = 1.0f / 120.0f;  // sun hour within 30 s of the game's: leave the game

enum Fault : int { FaultNone = 0, FaultStuck, FaultNotFinite, FaultWrite };

// ---- state --------------------------------------------------------------------------------------
std::mutex g_mu;  // table, info (the game thread only try_locks it)
apply::DayLengthConfig g_cfg;
apply::DayLengthTarget g_tgt;  // fixed before the callback is registered
apply::SunTimes g_game;        // fixed before the callback is registered
apply::SunTable g_table;
bool g_tableDirty = false;
DayLengthInfo g_info;
std::wstring g_tablePath;
uint32_t g_exeStamp = 0;
HANDLE g_thread = nullptr, g_stop = nullptr;
volatile LONG g_running = 0;

std::atomic<bool> g_armed{false};
std::atomic<int> g_fault{FaultNone};
std::atomic<float> g_sr{5.5f}, g_ss{19.5f};
std::atomic<float> g_hourSeen{-1.0f}, g_sunHourSeen{-1.0f};
std::atomic<uint64_t> g_writes{0};
// game thread only
float g_last[apply::kSunMaxFloats] = {};
bool g_hasLast = false, g_lastChanged = false;
int g_stuck = 0, g_writeFails = 0;

uint32_t exeTimeDateStamp() {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!b) return 0;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}

bool writeTextAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (fclose(f) == 0) && ok;
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string fieldId() { return g_tgt.field.text() + " " + apply::toString(g_tgt.kind); }

uintptr_t layerBase(const ComposeContext& c) {
    switch (g_tgt.field.layer) {
        case apply::Layer::Weather: return c.weather;
        case apply::Layer::Atmosphere: return c.atmosphere;
        case apply::Layer::Manager: return c.manager;
    }
    return 0;
}

void fault(int f) {
    g_armed = false;
    int expected = FaultNone;
    g_fault.compare_exchange_strong(expected, f);
}

// GAME thread, right after the compose: record the game's value at h, write the value at h'.
void postCompose(const ComposeContext& ctx) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    const float h = ctx.hour;
    if (!(h >= 0.0f && h <= 24.0f)) {  // unknown hour: never write
        g_hasLast = false;
        return;
    }
    const uintptr_t base = layerBase(ctx);
    if (!base) return;
    const uintptr_t addr = base + g_tgt.field.offset;
    const int n = g_tgt.field.count;
    const size_t bytes = size_t(n) * sizeof(float);
    float cur[apply::kSunMaxFloats] = {};
    if (!ctx.mem->read(addr, cur, bytes)) return;
    for (int k = 0; k < n; ++k)
        if (!std::isfinite(cur[k])) return fault(FaultNotFinite);

    const apply::SunTimes s{g_sr.load(std::memory_order_relaxed), g_ss.load(std::memory_order_relaxed)};
    const float hs = apply::remapHour(h, s, g_game);
    g_hourSeen.store(h, std::memory_order_relaxed);
    g_sunHourSeen.store(hs, std::memory_order_relaxed);
    const bool shifted = std::fabs(apply::hourDelta(hs, h)) > kMinShiftHours;

    float out[apply::kSunMaxFloats] = {};
    bool have = false;
    if (g_tgt.kind == apply::SunKind::Hour) {
        // the field must come back to the game's hour every tick (else it is not recomposed, or not an hour)
        if (std::fabs(apply::hourDelta(cur[0], h)) > 0.05f) {
            if (++g_stuck >= kStuckTicks) return fault(FaultStuck);
            return;
        }
        g_stuck = 0;
        out[0] = hs;
        have = shifted;
    } else {
        // our own value read back: the game did not recompose the field this tick
        const bool echo = g_hasLast && g_lastChanged && std::memcmp(cur, g_last, bytes) == 0;
        if (echo) {
            if (++g_stuck >= kStuckTicks) return fault(FaultStuck);
        } else {
            g_stuck = 0;
        }
        std::unique_lock<std::mutex> lk(g_mu, std::try_to_lock);
        if (!lk.owns_lock()) return;  // the worker is saving: skip one tick
        if (!echo && g_table.record(h, cur)) g_tableDirty = true;
        if (shifted) have = g_table.lookup(hs, out, g_cfg.minSamples, g_cfg.maxGapBins);
    }
    if (!have) {
        g_hasLast = false;
        return;
    }
    std::string why;
    if (!ctx.mem->write(addr, out, bytes, &why)) {
        g_hasLast = false;
        if (++g_writeFails >= kWriteFailLimit) fault(FaultWrite);
        return;
    }
    g_writeFails = 0;
    g_lastChanged = std::memcmp(out, cur, bytes) != 0;
    std::memcpy(g_last, out, bytes);
    g_hasLast = true;
    g_writes.fetch_add(1, std::memory_order_relaxed);
}

const char* faultText(int f) {
    switch (f) {
        case FaultStuck:
            return g_tgt.kind == apply::SunKind::Hour
                       ? "il campo (Kind=hour) non torna all'ora del gioco: non viene ricomposto o non è un'ora"
                       : "il gioco non ricompone il campo del sole (si legge sempre il valore scritto)";
        case FaultNotFinite: return "valori non finiti nel campo del sole";
        case FaultWrite: return "le scritture sul campo del sole continuano a fallire";
    }
    return "?";
}

void saveTable() {
    if (g_tgt.kind != apply::SunKind::SunDir || !g_info.fieldSet || g_tablePath.empty() || !g_exeStamp) return;
    std::string text;
    int bins = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_tableDirty) return;  // nothing new since the last save
        text = g_table.serialize(g_exeStamp, fieldId());
        bins = g_table.known(g_cfg.minSamples);
        g_tableDirty = false;
    }
    if (writeTextAtomic(g_tablePath, text)) QS_INFO("daylength: sun table saved (%d/%d bins known)", bins, apply::kSunBins);
    else QS_ERR("daylength: cannot save %s", narrow(g_tablePath).c_str());
}

void loadTable() {
    if (g_tgt.kind != apply::SunKind::SunDir || !g_info.fieldSet) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_table.reset(g_tgt.field.count);
    if (!g_exeStamp) {
        QS_ERR("daylength: exe TimeDateStamp not readable: the sun table is learned but not saved");
        return;
    }
    std::vector<uint8_t> raw;
    if (!readWholeFile(g_tablePath, raw)) {
        QS_INFO("daylength: no sun table yet: it is learned while playing (let a few game hours pass)");
        return;
    }
    std::string err;
    if (g_table.parse(std::string(raw.begin(), raw.end()), g_exeStamp, fieldId(), &err))
        QS_INFO("daylength: sun table loaded (%d/%d bins known)", g_table.known(g_cfg.minSamples), apply::kSunBins);
    else
        QS_INFO("daylength: sun table ignored (%s): learned again", err.c_str());
}

std::string hm(float h) { return apply::formatHour(h); }

DWORD WINAPI worker(LPVOID) {
    const apply::DayLengthConfig cfg = g_cfg;
    if (WaitForSingleObject(g_stop, cfg.startDelayMs) == WAIT_OBJECT_0) return 0;
    loadTable();
    const bool fieldSet = g_info.fieldSet;
    if (!fieldSet) featureSet(kKey, FeatureState::Off, "campo del sole non ancora trovato (probe)");

    apply::GameClock clock;
    bool registered = false, snapped = false, prevOn = false, loggedNoHook = false;
    std::string loggedTimes;
    const uint64_t started = GetTickCount64();
    uint64_t lastSave = started, lastLog = 0;
    float sr = g_game.sunrise, ss = g_game.sunset;
    while (WaitForSingleObject(g_stop, 250) != WAIT_OBJECT_0) {
        const uint64_t now = GetTickCount64();
        season::SeasonState st;
        season::SeasonConfig sc;
        {
            std::lock_guard<std::mutex> lk(shared().mutex);
            st = shared().season;
            sc = shared().config;
        }
        const int r = int(season::Region::Temperate);
        const bool on = cfg.enabled && sc.dayLengthControl && st.regionActive[r];
        const apply::SunTimes seasonal =
            apply::seasonalSunTimes(cfg, st.regionPhase[r], st.regionPhaseProgress[r], sc.transitionFraction);
        const apply::SunTimes target = on ? seasonal : g_game;

        // ramp on the game's clock (sleep or wait: straight to the target)
        WeatherInfo wi;
        const float hour = weatherGetInfo(wi) ? wi.hour : -1.0f;
        if (hour >= 0) {
            const apply::GameClock::Step step = clock.feed(hour);
            if (!snapped || step.jump) {
                sr = target.sunrise;
                ss = target.sunset;
                snapped = true;
            } else {
                const float maxStep = cfg.rampPerHour * float(step.hours);
                sr = apply::approach(sr, target.sunrise, maxStep);
                ss = apply::approach(ss, target.sunset, maxStep);
            }
        }
        g_sr = sr;
        g_ss = ss;

        // seasonal times in the log (every change of at least 5 minutes, and at start)
        const std::string times = hm(seasonal.sunrise) + "/" + hm(seasonal.sunset);
        if (times != loggedTimes) {
            loggedTimes = times;
            QS_INFO("daylength: seasonal sunrise %s, sunset %s (%.2f h of day, phase %s %.0f%%; game %s, %s)", hm(seasonal.sunrise).c_str(),
                    hm(seasonal.sunset).c_str(), seasonal.daylight(), season::toString(st.regionPhase[r]),
                    st.regionPhaseProgress[r] * 100.0f, hm(g_game.sunrise).c_str(), hm(g_game.sunset).c_str());
        }
        if (on != prevOn) {
            QS_INFO("daylength: seasonal day %s", on ? "on" : "off (F5 Durata del giorno, [DayLength] Enabled or region off)");
            prevOn = on;
        }

        // the after-compose callback
        if (fieldSet && !registered && !featureIsDisabled(kKey)) {
            if (weatherHookInstalled()) {
                registered = weatherAddPostCompose(&postCompose);
                if (registered) {
                    g_armed = true;
                    QS_INFO("daylength: writing %s after the compose", fieldId().c_str());
                } else {
                    featureDisable(kKey, "nessuno slot libero per l'aggancio dopo la composizione");
                }
            } else if (now - started > 120000 && !loggedNoHook) {
                loggedNoHook = true;
                featureSet(kKey, FeatureState::Off, "aggancio del meteo non attivo (vedi le righe weather: del log)");
                QS_ERR("daylength: the weather hook is not installed: nothing is written");
            }
        }
        const int f = g_fault.load();
        if (f != FaultNone && !featureIsDisabled(kKey)) {
            g_armed = false;
            featureDisable(kKey, faultText(f));
        }

        // status
        int bins = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            bins = g_table.known(cfg.minSamples);
            g_info.seasonal = seasonal;
            g_info.current = apply::SunTimes{sr, ss};
            g_info.tableBins = bins;
            g_info.writes = g_writes.load();
            g_info.hour = g_hourSeen.load();
            g_info.sunHour = g_sunHourSeen.load();
        }
        if (!featureIsDisabled(kKey) && registered) {
            if (on) featureSet(kKey, FeatureState::Active);
            else featureSet(kKey, FeatureState::Off, "spenta dal menu F5, da [DayLength] Enabled o regione spenta");
        }
        char det[160];
        if (fieldSet && g_tgt.kind == apply::SunKind::SunDir)
            snprintf(det, sizeof det, "alba %s, tramonto %s; tabella del sole %d/%d fasce", hm(sr).c_str(), hm(ss).c_str(), bins, apply::kSunBins);
        else
            snprintf(det, sizeof det, "alba %s, tramonto %s", hm(fieldSet ? sr : seasonal.sunrise).c_str(), hm(fieldSet ? ss : seasonal.sunset).c_str());
        featureDetail(kKey, det);

        if (cfg.logIntervalSec > 0 && now - lastLog >= uint64_t(cfg.logIntervalSec * 1000.0f)) {
            lastLog = now;
            const float hs = g_sunHourSeen.load();
            QS_INFO("daylength: %s; in use sunrise %s sunset %s, game hour %s -> sun hour %s, table %d/%d bins, %llu writes",
                    featureIsDisabled(kKey) ? "disabled" : (!fieldSet ? "no sun field (probe)" : (on ? "active" : "off")), hm(sr).c_str(),
                    hm(ss).c_str(), hour >= 0 ? hm(hour).c_str() : "?", hs >= 0 ? hm(hs).c_str() : "?", bins, apply::kSunBins,
                    (unsigned long long)g_writes.load());
        }
        if (now - lastSave >= uint64_t(cfg.saveIntervalSec * 1000.0f)) {
            lastSave = now;
            saveTable();
        }
    }
    // Stop writing, unregister, then save: the game composes its own sun on the next tick.
    g_armed = false;
    if (registered) weatherRemovePostCompose(&postCompose);
    Sleep(50);
    saveTable();
    QS_INFO("daylength: stop");
    return 0;
}

}  // namespace
}  // namespace daylengthimpl

bool daylengthStart(const std::wstring& scriptsDir, bool versionOk) {
    using namespace daylengthimpl;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return false;
    featureRegister(kKey, "Durata del giorno");
    apply::DayLengthConfig cfg;
    std::vector<std::string> warn;
    std::vector<uint8_t> raw;
    if (readWholeFile(joinPath(scriptsDir, L"QuattroStagioni.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        cfg.load(ini, warn);
    }
    apply::DayLengthTarget tgt;
    const std::wstring dataDir = joinPath(scriptsDir, L"QuattroStagioni");
    raw.clear();
    if (readWholeFile(joinPath(dataDir, L"targets.ini"), raw)) {
        Ini ini;
        ini.parse(std::string(raw.begin(), raw.end()));
        tgt.load(ini, warn);
    }
    for (const std::string& w : warn) QS_ERR("daylength: %s", w.c_str());
    g_cfg = cfg;
    g_tgt = tgt;
    g_game = apply::gameSunTimes(cfg);
    g_sr = g_game.sunrise;
    g_ss = g_game.sunset;
    g_tablePath = joinPath(dataDir, L"sun_table.ini");
    g_exeStamp = exeTimeDateStamp();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.started = true;
        g_info.fieldSet = tgt.enabled && tgt.field.valid;
        g_info.field = g_info.fieldSet ? fieldId() : std::string();
        g_info.game = g_game;
    }
    QS_INFO("daylength: start in %u ms (game day %s-%s, winter shift %+.2f/%+.2f h, min %.1f h of day, ramp %.2f h/h), field %s",
            cfg.startDelayMs, apply::formatHour(g_game.sunrise).c_str(), apply::formatHour(g_game.sunset).c_str(),
            cfg.sunriseShift[season::Phase::Winter], cfg.sunsetShift[season::Phase::Winter], cfg.minDaylightHours, cfg.rampPerHour,
            g_info.fieldSet ? g_info.field.c_str() : "not set ([daylength] in targets.ini)");
    if (!versionOk) {
        featureDisable(kKey, "version guard failed: the game is never written");
        g_running = 0;
        return false;
    }
    if (!cfg.enabled) {
        featureSet(kKey, FeatureState::Off, "[DayLength] Enabled=0");
        QS_INFO("daylength: [DayLength] Enabled=0: nothing is written");
        g_running = 0;
        return false;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        featureDisable(kKey, "cannot create the daylength thread");
        g_running = 0;
        return false;
    }
    return true;
}

void daylengthStop() {
    using namespace daylengthimpl;
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_running = 0;
}

bool daylengthGetInfo(DayLengthInfo& out) {
    using namespace daylengthimpl;
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_info;
    return g_info.started;
}

}  // namespace qs
