// QuattroStagioni - fake "game" classes for the end-to-end applier test (tests/apply_fake/run.sh),
// compiled with clang-cl (MSVC ABI) so they carry real MSVC RTTI and vtables like CrimsonDesert.exe.
// No headers and no CRT: only the extern "C" entry points are used by fake_game.cpp (mingw).
//
// Layouts (what the test targets.ini points at):
//   pa::TimeManager     +0x08 hour f32, +0x0C day u32, +0x10 day speed f32, +0x14 night speed f32
//                       (advance(): the clock runs at day speed from 03:00 to 19:00, night speed otherwise)
//   pa::WeatherManager  +0x31 manual u8, +0x34 climate temperature f32 (object check),
//                       +0x38 WeatherState flags u32, +0x40 snow amount f32, +0x48 precipitation f32,
//                       +0x4C a field the game rewrites every frame (must be detected and given up)
//   pa::LightManager    +0x10/+0x14/+0x18 tint r/g/b f32, +0x1C sun elevation offset f32 (degrees);
//                       update() is slot 1, captured by the applier through a vtable slot swap

inline void* operator new(unsigned long long, void* p) noexcept { return p; }

namespace pa {

class TimeManager {
public:
    virtual int reserved0() const { return 0; }
    virtual void advance(float dt) {  // slot 1
        bool day = hour >= 3.0f && hour < 19.0f;
        hour += dt * (day ? daySpeed : nightSpeed);
        if (hour >= 24.0f) {
            hour -= 24.0f;
            ++dayCounter;
        }
    }
    float hour = 12.0f;        // +0x08
    unsigned dayCounter = 1;   // +0x0C
    float daySpeed = 1.0f;     // +0x10
    float nightSpeed = 1.0f;   // +0x14
};

class WeatherManager {
public:
    virtual int kind() const { return 2; }
    virtual void tick(float dt) {  // slot 1
        (void)dt;
        pinned = 0.25f;            // the game owns this field: the applier must give up on it
    }
    char pad08[0x31 - 0x08] = {};
    unsigned char manual = 0;      // +0x31
    unsigned char pad32[2] = {};
    float climateTemperature = 4.0f;  // +0x34
    unsigned flags = 0x100 | 0x20;    // +0x38 (0x100: a game bit outside the mod's mask; 0x20 Sunny)
    float pad3c = 0;
    float snowAmount = 0.0f;       // +0x40
    float snowRate = 0.0f;         // +0x44
    float precipitation = 0.0f;    // +0x48
    float pinned = 0.25f;          // +0x4C
};

class LightManager {
public:
    virtual int reserved0() const { return 0; }
    virtual void update(float dt) { calls += dt > 0 ? 1u : 0u; }  // slot 1
    unsigned calls = 0;            // +0x08
    unsigned pad = 0;
    float tint[3] = {1.0f, 0.9f, 0.8f};  // +0x10
    float sunElevation = 0.0f;     // +0x1C
};

}  // namespace pa

extern "C" {
unsigned long long fake_time_size() { return sizeof(pa::TimeManager); }
unsigned long long fake_weather_size() { return sizeof(pa::WeatherManager); }
unsigned long long fake_light_size() { return sizeof(pa::LightManager); }
void* fake_make_time(void* mem) { return new (mem) pa::TimeManager(); }
void* fake_make_weather(void* mem) { return new (mem) pa::WeatherManager(); }
void* fake_make_light(void* mem) { return new (mem) pa::LightManager(); }
void fake_time_advance(void* o, float dt) { static_cast<pa::TimeManager*>(o)->advance(dt); }
void fake_weather_tick(void* o, float dt) { static_cast<pa::WeatherManager*>(o)->tick(dt); }
void fake_light_update(void* o, float dt) { static_cast<pa::LightManager*>(o)->update(dt); }
}
