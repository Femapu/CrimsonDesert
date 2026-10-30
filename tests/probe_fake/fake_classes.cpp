// QuattroStagioni - fake "game" classes compiled with clang-cl (MSVC ABI) so they carry real MSVC
// RTTI (TypeDescriptor / CompleteObjectLocator / ClassHierarchyDescriptor) and MSVC vtables, like
// CrimsonDesert.exe. No headers and no CRT: only extern "C" entry points are used by fake_game.cpp
// (built with mingw). Build: tests/probe_fake/run.sh.
//
// Field offsets deliberately mimic what docs/recon/probe_plan.md looks for:
//   PAWeatherManager: +0x31 manual flag, +0x34 climate temperature, +0x44 current temperature,
//                     +0x48 snow 0..1, +0x4C accumulated dt (checks the capture stub keeps xmm1)
//   TimeOfDay:        +0x08 hour 0..24 (float), +0x0C day counter, +0x10 total advanced (double)

inline void* operator new(unsigned long long, void* p) noexcept { return p; }

namespace pa {

struct ManagerBase {
    virtual int kind() const { return 1; }
    virtual void tick(float dt) { (void)dt; }
};

class PAWeatherManager : public ManagerBase {
public:
    int kind() const override { return 2; }
    void tick(float dt) override {
        accum += dt;
        snow += dt * 0.01f;
        if (snow > 1.0f) snow = 0.0f;
        currentTemperature = climateTemperature - 10.0f * snow;
    }
    virtual float getSnow() const { return snow; }

    char pad08[0x31 - 0x08] = {};
    unsigned char manual = 0;              // +0x31
    unsigned char pad32[2] = {};
    float climateTemperature = 4.0f;       // +0x34
    float pad38[3] = {};
    float currentTemperature = 4.0f;       // +0x44
    float snow = 0.0f;                     // +0x48
    float accum = 0.0f;                    // +0x4C
};

class TimeOfDay {
public:
    virtual int reserved0() const { return 0; }
    virtual void advance(float dt) {       // slot 1 (+0x08)
        hour += dt;
        total += dt;
        if (hour >= 24.0f) {
            hour -= 24.0f;
            ++day;
        }
    }
    float hour = 6.0f;                     // +0x08
    unsigned day = 1;                      // +0x0C
    double total = 0.0;                    // +0x10
};

namespace scriptComponent {
class weatherTimeSimulatorScriptComponent : public TimeOfDay {
public:
    void advance(float dt) override { TimeOfDay::advance(dt * 2.0f); }
};
}  // namespace scriptComponent

}  // namespace pa

extern "C" {
unsigned long long fake_weather_size() { return sizeof(pa::PAWeatherManager); }
unsigned long long fake_tod_size() { return sizeof(pa::TimeOfDay); }
void* fake_make_weather(void* mem) { return new (mem) pa::PAWeatherManager(); }
void* fake_make_tod(void* mem) { return new (mem) pa::TimeOfDay(); }
void* fake_make_sim(void* mem) { return new (mem) pa::scriptComponent::weatherTimeSimulatorScriptComponent(); }
// virtual dispatch through the object's vtable (what a slot swap intercepts)
void fake_tick(void* obj, float dt) { static_cast<pa::ManagerBase*>(obj)->tick(dt); }
void fake_advance(void* obj, float dt) { static_cast<pa::TimeOfDay*>(obj)->advance(dt); }
float fake_get_snow(void* obj) { return static_cast<pa::PAWeatherManager*>(obj)->getSnow(); }
float fake_hour(void* obj) { return static_cast<pa::TimeOfDay*>(obj)->hour; }
double fake_total(void* obj) { return static_cast<pa::TimeOfDay*>(obj)->total; }
float fake_accum(void* obj) { return static_cast<pa::PAWeatherManager*>(obj)->accum; }
}
