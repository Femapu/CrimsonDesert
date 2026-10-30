// QuattroStagioni - fake "game" exe for the end-to-end probe test under wine (tests/probe_fake/run.sh).
//
// Built with mingw and linked with fake_classes.obj (clang-cl, MSVC RTTI). It mimics the shapes the
// real probes look for: RTTI classes with heap instances, a global clock struct reachable by a
// RIP-relative signature, a global world pointer chain, a plain function hooked with MinHook, and an
// extra executable section ".prot" standing in for the Denuvo section, entered through a 5-byte jmp
// stub. It loads qs_probe_test.dll (the real probe code), runs a frame loop, then verifies that the
// game state is exactly what it would be without probes (the capture stubs must not disturb
// arguments) and that every hook was restored.
//
// Usage: fake_game.exe <probes.ini> <probe.log>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
// symbols the MSVC-ABI object needs (no MSVC CRT here)
int _fltused = 0;

unsigned long long fake_weather_size();
unsigned long long fake_tod_size();
void* fake_make_weather(void* mem);
void* fake_make_tod(void* mem);
void* fake_make_sim(void* mem);
void fake_tick(void* obj, float dt);
void fake_advance(void* obj, float dt);
float fake_hour(void* obj);
double fake_total(void* obj);
float fake_accum(void* obj);

struct FakeGameTime {  // same layout as the real GameTime (data/partial/signatures_2040_hotfix.json)
    uint32_t day, hour, minute, second, millisecond;
    uint8_t weekday, pad;
    uint16_t rate;
    uint64_t stamp;
};
FakeGameTime g_fakeClock = {1, 6, 0, 0, 0, 1, 0, 24, 0};
void* g_world = nullptr;

void fake_clock_anchor();
void fake_world_anchor();
void fake_weather_update(void* wm, float dt);
void fake_stub();
int protected_body(int x);

// game function hooked by MinHook in the test: dt arrives in xmm1 and must survive the capture stub
void fake_weather_update_impl(void* wm, float dt) { fake_tick(wm, dt); }

__attribute__((section(".prot"), noinline)) int protected_body(int x) { return x * 3 + 1; }
}

// Unique 10-byte markers (movabs) make the signatures unambiguous.
asm(R"(
    .data
    .globl "??_7type_info@@6B@"
"??_7type_info@@6B@":
    .quad 0, 0
    .text
    .globl fake_clock_anchor
fake_clock_anchor:
    movabs $0x1122334455667788, %rax
    lea g_fakeClock(%rip), %rax
    ret
    .globl fake_world_anchor
fake_world_anchor:
    movabs $0x99AABBCCDDEEFF00, %rax
    mov g_world(%rip), %rax
    ret
    .globl fake_weather_update
fake_weather_update:
    movabs $0x0BADC0DE5EA50A11, %rax
    jmp fake_weather_update_impl
    .globl fake_stub
fake_stub:
    jmp protected_body
)");

typedef bool (*StartFn)(const wchar_t*, const wchar_t*);
typedef void (*StopFn)();
typedef int (*SelfTestFn)(void*, void*, void*, char*, int);

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (c) ++g_pass;                                                          \
        else { ++g_fail; std::printf("FAKE GAME FAIL line %d: %s\n", __LINE__, #c); } \
    } while (0)

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: fake_game <probes.ini> <probe.log>\n");
        return 2;
    }
    void* wm = fake_make_weather(std::calloc(1, fake_weather_size()));
    void* tod = fake_make_tod(std::calloc(1, fake_tod_size()));
    void* sim = fake_make_sim(std::calloc(1, fake_tod_size()));
    void** world = static_cast<void**>(std::calloc(1, 0x100));
    world[0x60 / 8] = wm;
    g_world = world;
    void** todVt = *static_cast<void***>(tod);
    void* todSlot1 = todVt[1];
    uint8_t fnHead[5];
    std::memcpy(fnHead, reinterpret_cast<void*>(&fake_weather_update), 5);

    HMODULE dll = LoadLibraryW(L"qs_probe_test.dll");
    CHECK(dll != nullptr);
    if (!dll) return 1;
    StartFn start = reinterpret_cast<StartFn>((void*)GetProcAddress(dll, "qsTestStart"));
    StopFn stop = reinterpret_cast<StopFn>((void*)GetProcAddress(dll, "qsTestStop"));
    SelfTestFn self = reinterpret_cast<SelfTestFn>((void*)GetProcAddress(dll, "qsTestSelfTest"));
    CHECK(start && stop && self);
    if (!start || !stop || !self) return 1;
    CHECK(start(argv[1], argv[2]));

    void (*volatile update)(void*, float) = &fake_weather_update;
    const int frames = 400;
    bool sawSwap = false, sawDetour = false;
    for (int i = 0; i < frames; ++i) {
        fake_advance(tod, 0.25f);  // virtual call through tod's vtable slot 1
        fake_advance(sim, 0.25f);  // derived class: its own vtable, never hooked
        update(wm, 0.5f);          // MinHook target
        g_fakeClock.millisecond = (i % 100) * 10;
        g_fakeClock.hour = 6 + (i / 40) % 18;
        g_fakeClock.stamp = GetTickCount64();
        if (todVt[1] != todSlot1) sawSwap = true;
        if (std::memcmp(reinterpret_cast<void*>(&fake_weather_update), fnHead, 5) != 0) sawDetour = true;
        Sleep(10);
    }
    char buf[4096] = {};
    int selfFails = self(reinterpret_cast<void*>(&protected_body), reinterpret_cast<void*>(&fake_stub),
                         reinterpret_cast<void*>(&fake_weather_update_impl), buf, sizeof(buf));
    std::printf("%s", buf);
    CHECK(selfFails == 0);
    stop();

    // state must be exactly what the game computes without probes
    double expectTotal = 0.25 * frames;
    CHECK(std::fabs(fake_total(tod) - expectTotal) < 1e-9);
    CHECK(std::fabs(fake_total(sim) - 2 * expectTotal) < 1e-9);
    CHECK(std::fabs(double(fake_accum(wm)) - 0.5 * frames) < 1e-3);
    CHECK(sawSwap);    // the capture probe really swapped the slot
    CHECK(sawDetour);  // and really detoured the function
    CHECK(todVt[1] == todSlot1);  // restored
    CHECK(std::memcmp(reinterpret_cast<void*>(&fake_weather_update), fnHead, 5) == 0);  // unhooked
    CHECK(protected_body(2) == 7);
    fake_advance(tod, 1.0f);
    CHECK(std::fabs(fake_total(tod) - expectTotal - 1.0) < 1e-9);
    std::printf("fake game: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
