// QuattroStagioni - test DLL wrapping the real applier for the fake game (tests/apply_fake/run.sh).
// Exports start/stop with explicit paths, a way to set the SeasonState (real season engine), the
// game clock reader, a status dump and a self test of the write guard against the fake exe.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/core/season/season.h"
#include "../../src/win/apply/apply.h"
#include "../../src/win/apply/guarded_memory.h"
#include "../../src/win/log.h"
#include "../../src/win/probe/module.h"
#include "../../src/win/qs_state.h"

struct Status {  // same layout as in fake_game.cpp
    int features[6];
    int clockKnown;
    int gameClockHooked;
    double gameHour, gameDay;
    char text[4096];
};
struct SeasonInfo {
    float dayScale, nightScale, snowCover, tintB, sunOffset, tempOffset;
};

extern "C" __declspec(dllexport) bool qsApplyTestStart(const wchar_t* ini, const wchar_t* log) {
    qs::log::open(log);
    qs::log::setLevel(2);
    return qs::applyStartWith(ini);
}

extern "C" __declspec(dllexport) void qsApplyTestStop() {
    qs::applyStop();
    qs::log::close();
}

extern "C" __declspec(dllexport) void qsApplySetSeason(double day, SeasonInfo* out) {
    qs::season::SeasonConfig cfg;
    cfg.setDefaults();
    qs::season::Engine e;
    e.configure(cfg);
    e.update(day, -1.0f);
    const qs::season::SeasonState& s = e.state();
    {
        std::lock_guard<std::mutex> lk(qs::shared().mutex);
        qs::shared().config = cfg;
        qs::shared().season = s;
    }
    out->dayScale = s.dayScale;
    out->nightScale = s.nightScale;
    out->snowCover = s.snowCover[0];
    out->tintB = s.lightTint[2];
    out->sunOffset = s.sunElevationOffsetDeg;
    out->tempOffset = s.dailyMeanTempC[0] - (cfg.regions[0].meanTempC + cfg.regions[0].ampTempC);
}

extern "C" __declspec(dllexport) int qsApplyReadClock(double* day, double* hours) {
    return qs::applyReadGameClock(*day, *hours) ? 1 : 0;
}

extern "C" __declspec(dllexport) void qsApplyStatus(Status* out) {
    using namespace qs::apply;
    ApplyStatus st;
    std::memset(out, 0, sizeof(*out));
    if (!qs::applyGetStatus(st)) return;
    for (int f = 0; f < kFeatureCount && f < 6; ++f) out->features[f] = int(st.features[f].state);
    out->clockKnown = st.clockKnown;
    out->gameClockHooked = qs::shared().gameClockHooked.load();
    out->gameHour = st.gameHour;
    out->gameDay = st.gameDay;
    std::string s = "status: " + st.phase + ", weather " + toString(st.weather.current) + "\n";
    for (int f = 0; f < kFeatureCount; ++f)
        s += std::string("  feature ") + toString(Feature(f)) + ": " + toString(st.features[f].state) + "\n";
    for (const TargetStatus& t : st.targets) {
        char b[256];
        std::snprintf(b, sizeof b, "  target %-12s %-8s original %-9g current %-9g anomalies %u %s\n", t.name.c_str(), toString(t.state),
                      t.original, t.current, t.anomalies, t.error.c_str());
        s += b;
    }
    std::snprintf(out->text, sizeof out->text, "%s", s.c_str());
}

extern "C" __declspec(dllexport) int qsApplySelfTest(void* code, void* rdata, void* data, char* out, int outLen) {
    using namespace qs::apply;
    std::string s;
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        s += ok ? "  selftest ok   " : "  selftest FAIL ";
        s += what;
        s += "\n";
        if (!ok) ++fails;
    };
    qs::probe::ModuleInfo m;
    std::string err;
    check(qs::probe::describeModule(nullptr, m, &err), "describeModule");
    check(checkWritable(m, uintptr_t(code), 4) == WriteCheck::NotReadWrite, "code refused");
    check(checkWritable(m, uintptr_t(rdata), 4) == WriteCheck::NotReadWrite, ".rdata refused");
    check(checkWritable(m, uintptr_t(data), 4) == WriteCheck::Ok, ".data accepted");
    void* heap = std::malloc(64);
    check(checkWritable(m, uintptr_t(heap), 8) == WriteCheck::Ok, "heap accepted");
    static int ownGlobal = 0;
    check(checkWritable(m, uintptr_t(&ownGlobal), 4) == WriteCheck::OtherModule, "other module's data refused");
    check(checkWritable(m, 0x10, 4) == WriteCheck::Unmapped, "null page refused");
    GuardedMemory gm(m);
    float v = 9.0f, back = 0;
    std::string why;
    check(!gm.write(uint64_t(uintptr_t(rdata)), &v, 4, &why) && gm.read(uint64_t(uintptr_t(rdata)), &back, 4) && back == 0.5f,
          "GuardedMemory refuses .rdata, value unchanged");
    check(gm.write(uint64_t(uintptr_t(heap)), &v, 4, &why) && *static_cast<float*>(heap) == 9.0f, "GuardedMemory writes the heap");
    std::free(heap);
    char head[64];
    std::snprintf(head, sizeof(head), "selftest: %d failure(s)\n", fails);
    s = head + s;
    if (out && outLen > 0) std::snprintf(out, size_t(outLen), "%s", s.c_str());
    return fails;
}
