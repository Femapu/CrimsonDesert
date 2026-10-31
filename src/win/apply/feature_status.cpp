// QuattroStagioni - status of the "after compose" features (see feature_status.h).
#include "feature_status.h"

#include <mutex>

#include "../log.h"

namespace qs {
namespace {
std::mutex g_mu;
std::vector<FeatureStatus> g_list;

FeatureStatus& slot(const char* key) {
    for (FeatureStatus& f : g_list)
        if (f.key == key) return f;
    g_list.push_back(FeatureStatus());
    g_list.back().key = key;
    g_list.back().label = key;
    return g_list.back();
}
}  // namespace

void featureRegister(const char* key, const char* label) {
    std::lock_guard<std::mutex> lk(g_mu);
    slot(key).label = label ? label : key;
}

void featureSet(const char* key, FeatureState s, const std::string& reason) {
    std::lock_guard<std::mutex> lk(g_mu);
    FeatureStatus& f = slot(key);
    if (f.state == FeatureState::Disabled) return;
    f.state = s;
    f.reason = reason;
}

void featureDetail(const char* key, const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_mu);
    slot(key).detail = detail;
}

bool featureDisable(const char* key, const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        FeatureStatus& f = slot(key);
        if (f.state == FeatureState::Disabled) return false;
        f.state = FeatureState::Disabled;
        f.reason = why;
    }
    QS_ERR("%s: DISABLED: %s", key, why.c_str());
    return false;
}

bool featureIsDisabled(const char* key) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const FeatureStatus& f : g_list)
        if (f.key == key) return f.state == FeatureState::Disabled;
    return false;
}

std::vector<FeatureStatus> featureList() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_list;
}

}  // namespace qs
