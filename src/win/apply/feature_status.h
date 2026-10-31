// QuattroStagioni - status of the "after compose" features for the F5 Info tab and the log.
//
// Each feature (lightning, lighting, daylength, particles) reports here; the menu lists every
// registered feature in the Info tab. featureDisable() writes "<key>: DISABLED: <why>" to
// QuattroStagioni.log once and keeps the reason (same convention as "gpu:" and "weather:").
#pragma once
#include <string>
#include <vector>

namespace qs {

enum class FeatureState : unsigned char { Waiting, Active, Off, Disabled };

struct FeatureStatus {
    std::string key;     // "lightning", ... (log prefix)
    std::string label;   // Italian label for the menu (translated with tr() by the menu)
    FeatureState state = FeatureState::Waiting;
    std::string reason;  // why Off / Disabled
    std::string detail;  // one short live line (e.g. "strikes 3, last 12 s ago")
};

// Registers (or updates the label of) a feature; state Waiting.
void featureRegister(const char* key, const char* label);
// Active / Off / Waiting with an optional reason; never leaves Disabled.
void featureSet(const char* key, FeatureState s, const std::string& reason = std::string());
void featureDetail(const char* key, const std::string& detail);
// Disabled with a log line "<key>: DISABLED: <why>" (only the first time). Returns false (handy for `return`).
bool featureDisable(const char* key, const std::string& why);
bool featureIsDisabled(const char* key);
std::vector<FeatureStatus> featureList();  // registration order

}  // namespace qs
