// QuattroStagioni - the game builds this release is made for (single source of truth).
//
// Used by the plugin's version guard (src/win/setup.cpp) and by the installer (src/cli/qs_install.cpp).
// After a game patch: add the new executable here (size in bytes and PE TimeDateStamp, both printed by
// the plugin log line "version check: exe size ... timestamp ..." and by `qs_install --check`), check
// meta/0.paver, rebuild and release.
#pragma once
#include <cstdint>

namespace qs::builds {

struct KnownExe {
    uint64_t size;       // bin64/CrimsonDesert.exe file size in bytes
    uint32_t timestamp;  // PE header TimeDateStamp
    const char* label;   // human readable build name
};

// Game version string written in manifests and shown to the user.
inline constexpr const char* kGameVersion = "2.04.00";

// meta/0.paver of 2.04.00 with "Charting the Unknown":
// u16 2, u16 4, u16 0, u32 1, {u32 6, "dlc_01", u32 0}, u32 hashlittle(paver[0:24])
inline constexpr const char* kPaverHex = "0200040000000100000006000000646c635f30310000000047396a31";

// Known 2.04.00 executables (Steam): launch build (2026-10-29) and the hotfix exe of 2026-10-30.
// The LAST entry is the most recent build (shown as "expected" in logs).
inline constexpr KnownExe kKnownExes[] = {
    {333779352ull, 1793246886u, "2.04.00 (lancio, 29/10/2026)"},
    {336264088ull, 1793296370u, "2.04.00 hotfix (30/10/2026)"},
};
inline constexpr unsigned kKnownExeCount = sizeof(kKnownExes) / sizeof(kKnownExes[0]);
inline constexpr const KnownExe& kLatestExe = kKnownExes[kKnownExeCount - 1];

// Steam app id of Crimson Desert (used to find the game through Steam's library folders).
inline constexpr const char* kSteamAppId = "3321460";

inline const KnownExe* findKnownExe(uint64_t size, uint32_t timestamp) {
    for (const KnownExe& k : kKnownExes)
        if (k.size == size && k.timestamp == timestamp) return &k;
    return nullptr;
}

}  // namespace qs::builds
