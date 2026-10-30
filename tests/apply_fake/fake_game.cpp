// QuattroStagioni - fake "game" exe for the end-to-end applier test under wine (tests/apply_fake/run.sh).
//
// Built with mingw and linked with fake_classes.obj (clang-cl, MSVC RTTI). It loads
// qs_apply_test.dll (the real applier + probe helpers + season engine), sets a winter SeasonState,
// runs a frame loop with a time manager (found through a global pointer chain), a weather manager
// (heap scan by vtable, replaced by a new instance halfway: the applier must notice the vtable
// change and never write the old block again), a light manager (captured through a vtable slot), a
// GameTime-like clock struct in the exe's .data (read only), a float in .data (written) and a const
// float in .rdata (writes must be refused). Then it checks the seasonal values, the limits, the
// rate limit, the restore on stop and the status.
//
// Usage: fake_game.exe <targets.ini> <QuattroStagioni.log>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int _fltused = 0;

unsigned long long fake_time_size();
unsigned long long fake_weather_size();
unsigned long long fake_light_size();
void* fake_make_time(void* mem);
void* fake_make_weather(void* mem);
void* fake_make_light(void* mem);
void fake_time_advance(void* o, float dt);
void fake_weather_tick(void* o, float dt);
void fake_light_update(void* o, float dt);

struct FakeGameTime {
    uint32_t day, hour, minute, second;
};
FakeGameTime g_fakeClock = {3, 6, 0, 0};
void* g_world = nullptr;
float g_globalTemp = 1.5f;              // .data: written by the applier (temperature_offset)
extern const float g_constScale;        // .rdata: writes must be refused
const float g_constScale = 0.5f;

void fake_world_anchor();
void fake_clock_anchor();
void fake_global_anchor();
void fake_const_anchor();
}

// Unique 10-byte markers (movabs) make the signatures unambiguous.
asm(R"(
    .data
    .globl "??_7type_info@@6B@"
"??_7type_info@@6B@":
    .quad 0, 0
    .text
    .globl fake_world_anchor
fake_world_anchor:
    movabs $0x99AABBCCDDEEFF01, %rax
    mov g_world(%rip), %rax
    ret
    .globl fake_clock_anchor
fake_clock_anchor:
    movabs $0x1122334455667701, %rax
    lea g_fakeClock(%rip), %rax
    ret
    .globl fake_global_anchor
fake_global_anchor:
    movabs $0x0BADC0DE5EA50A01, %rax
    lea g_globalTemp(%rip), %rax
    ret
    .globl fake_const_anchor
fake_const_anchor:
    movabs $0x0BADC0DE5EA50A02, %rax
    lea g_constScale(%rip), %rax
    ret
)");

struct Status {  // filled by qsApplyStatus (same layout in shim.cpp)
    int features[6];      // apply::FeatureState per apply::Feature
    int clockKnown;
    int gameClockHooked;
    double gameHour, gameDay;
    char text[4096];
};
struct SeasonInfo {  // filled by qsApplySetSeason
    float dayScale, nightScale, snowCover, tintB, sunOffset, tempOffset;
};

typedef bool (*StartFn)(const wchar_t*, const wchar_t*);
typedef void (*StopFn)();
typedef void (*SeasonFn)(double, SeasonInfo*);
typedef int (*ClockFn)(double*, double*);
typedef void (*StatusFn)(Status*);
typedef int (*SelfTestFn)(void*, void*, void*, char*, int);

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (c) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAKE GAME FAIL line %d: %s\n", __LINE__, #c); }   \
    } while (0)
static bool approx(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

template <class T>
static T fld(void* o, size_t off) {
    T v;
    std::memcpy(&v, static_cast<uint8_t*>(o) + off, sizeof(T));
    return v;
}
template <class T>
static void setFld(void* o, size_t off, T v) {
    std::memcpy(static_cast<uint8_t*>(o) + off, &v, sizeof(T));
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: fake_game <targets.ini> <log>\n");
        return 2;
    }
    void* tm = fake_make_time(std::calloc(1, fake_time_size()));
    void* wm = fake_make_weather(std::calloc(1, fake_weather_size()));
    void* lm = fake_make_light(std::calloc(1, fake_light_size()));
    void** world = static_cast<void**>(std::calloc(1, 0x100));
    world[0x60 / 8] = tm;
    g_world = world;
    void* lightSlot1 = (*static_cast<void***>(lm))[1];

    HMODULE dll = LoadLibraryW(L"qs_apply_test.dll");
    CHECK(dll != nullptr);
    if (!dll) return 1;
    StartFn start = reinterpret_cast<StartFn>((void*)GetProcAddress(dll, "qsApplyTestStart"));
    StopFn stop = reinterpret_cast<StopFn>((void*)GetProcAddress(dll, "qsApplyTestStop"));
    SeasonFn setSeason = reinterpret_cast<SeasonFn>((void*)GetProcAddress(dll, "qsApplySetSeason"));
    ClockFn readClock = reinterpret_cast<ClockFn>((void*)GetProcAddress(dll, "qsApplyReadClock"));
    StatusFn status = reinterpret_cast<StatusFn>((void*)GetProcAddress(dll, "qsApplyStatus"));
    SelfTestFn self = reinterpret_cast<SelfTestFn>((void*)GetProcAddress(dll, "qsApplySelfTest"));
    CHECK(start && stop && setSeason && readClock && status && self);
    if (!start || !stop || !setSeason || !readClock || !status || !self) return 1;

    SeasonInfo si{};
    setSeason(33.5, &si);  // mid winter
    std::printf("season: dayScale %.3f nightScale %.3f snow %.3f tintB %.3f sunOffset %.2f tempOffset %.2f\n", si.dayScale,
                si.nightScale, si.snowCover, si.tintB, si.sunOffset, si.tempOffset);
    CHECK(si.dayScale > 1.6f && si.nightScale < 0.6f && si.snowCover > 0.05f && si.tintB != 1.0f);
    CHECK(start(argv[1], argv[2]));

    const float kDayMax = 1.6f, kNightMin = 0.6f, kRate = 0.5f;  // as in targets_test.ini
    uint64_t firstChange = 0;
    bool rateOk = true, sawReplacement = false;
    double clockDay = -1, clockHours = 0;
    int clockCalls = 0;
    void* oldWm = nullptr;
    const int frames = 700;
    for (int i = 0; i < frames; ++i) {
        fake_time_advance(tm, 0.01f);
        fake_weather_tick(wm, 0.01f);
        fake_light_update(lm, 0.01f);
        // GameTime-like clock: 6 game minutes per frame
        g_fakeClock.minute += 6;
        if (g_fakeClock.minute >= 60) g_fakeClock.minute -= 60, ++g_fakeClock.hour;
        if (g_fakeClock.hour >= 24) g_fakeClock.hour = 0, ++g_fakeClock.day;
        // rate limit of the day speed, observed from the game side
        float ds = fld<float>(tm, 0x10);
        uint64_t now = GetTickCount64();
        if (ds != 1.0f && !firstChange) firstChange = now;
        if (firstChange && std::fabs(ds - 1.0f) > kRate * double(now - firstChange + 200) / 1000.0 + 0.01) rateOk = false;
        if (i == 350) {
            // the weather manager is destroyed and a new one is created elsewhere
            oldWm = wm;
            wm = fake_make_weather(std::calloc(1, fake_weather_size()));
            *static_cast<void**>(oldWm) = nullptr;  // destructor: vtable gone
            setFld<float>(oldWm, 0x40, 7.0f);         // block reused by someone else
            setFld<unsigned char>(oldWm, 0x31, 0x77);
            sawReplacement = true;
        }
        if (i % 50 == 49) {
            double d, h;
            if (readClock(&d, &h)) {
                clockDay = d;
                clockHours += h;
                ++clockCalls;
            }
        }
        Sleep(10);
    }
    char buf[4096] = {};
    int selfFails = self(reinterpret_cast<void*>(&fake_world_anchor), reinterpret_cast<void*>(const_cast<float*>(&g_constScale)),
                         reinterpret_cast<void*>(&g_globalTemp), buf, sizeof(buf));
    std::printf("%s", buf);
    CHECK(selfFails == 0);

    // ---- live values (winter)
    Status st{};
    status(&st);
    std::printf("%s", st.text);
    std::printf("time manager: day speed %.4f night speed %.4f\n", fld<float>(tm, 0x10), fld<float>(tm, 0x14));
    CHECK(approx(fld<float>(tm, 0x10), kDayMax));        // 1.0 * dayScale (~1.75), clamped to Max
    CHECK(approx(fld<float>(tm, 0x14), kNightMin));      // 1.0 * nightScale (~0.54), clamped to Min
    CHECK(rateOk && firstChange != 0);                   // never faster than MaxRate
    CHECK(approx(fld<float>(wm, 0x40), si.snowCover, 1e-3));  // the NEW weather manager got the snow
    CHECK(fld<unsigned char>(wm, 0x31) == 1);            // manual weather on
    unsigned fl = fld<unsigned>(wm, 0x38);
    CHECK((fl & 0x100) != 0 && (fl & 0x3F) != 0);         // game bit preserved, one weather flag set
    CHECK(approx(fld<float>(wm, 0x4C), 0.25f));            // game-owned field left to the game
    CHECK(sawReplacement && approx(fld<float>(oldWm, 0x40), 7.0f) && fld<unsigned char>(oldWm, 0x31) == 0x77);  // old block untouched
    CHECK(approx(fld<float>(lm, 0x18), 0.8 * si.tintB, 1e-4));  // tint B scaled
    CHECK(approx(fld<float>(lm, 0x10), 1.0 * 1.0, 0.2));        // tint R scaled from 1.0 (close to 1)
    CHECK(approx(fld<float>(lm, 0x1C), si.sunOffset, 1e-3));    // sun elevation offset (Mode=offset from 0)
    CHECK(approx(g_globalTemp, 1.5 + si.tempOffset, 1e-3));     // .data global written (Mode=offset)
    CHECK(approx(g_constScale, 0.5f));                           // .rdata never written
    CHECK((*static_cast<void***>(lm))[1] == lightSlot1);       // capture hook removed after the capture
    CHECK(st.features[0] == 2 && st.features[1] == 2 && st.features[2] == 2 && st.features[3] == 2);  // weather snow light daylength active
    CHECK(st.features[4] == 3);                                // particles: the .rdata target is in error
    CHECK(st.features[5] == 2 && st.clockKnown && st.gameClockHooked);  // clock read
    CHECK(clockCalls >= 8 && clockHours > 30.0 && clockDay >= 4.0);   // ~6 game minutes per frame
    std::printf("clock: day %.3f, %.1f game hours in %d calls\n", clockDay, clockHours, clockCalls);

    stop();

    // ---- restored on stop
    status(&st);
    CHECK(approx(fld<float>(tm, 0x10), 1.0f) && approx(fld<float>(tm, 0x14), 1.0f));
    CHECK(approx(fld<float>(wm, 0x40), 0.0f) && fld<unsigned char>(wm, 0x31) == 0);
    CHECK(fld<unsigned>(wm, 0x38) == (0x100u | 0x20u));
    CHECK(approx(fld<float>(lm, 0x10), 1.0f) && approx(fld<float>(lm, 0x14), 0.9f) && approx(fld<float>(lm, 0x18), 0.8f));
    CHECK(approx(fld<float>(lm, 0x1C), 0.0f));
    CHECK(approx(g_globalTemp, 1.5f));
    CHECK(approx(fld<float>(oldWm, 0x40), 7.0f));
    CHECK(!st.gameClockHooked);
    std::printf("fake game: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
