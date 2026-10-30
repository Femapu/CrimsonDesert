// QuattroStagioni - manifest parsing.
#include "manifest.h"

namespace qs {

Manifest parseManifest(const Ini& ini, std::vector<std::string>& errors) {
    Manifest m;
    m.gameVersion = ini.get("game", "version");
    for (const std::string& sec : ini.sections()) {
        if (sec.rfind("entry.", 0) != 0) continue;
        ManifestEntry e;
        e.id = sec.substr(6);
        e.enabled = ini.getInt(sec, "enabled", 1) != 0;
        e.path = ini.get(sec, "path");
        e.group = ini.get(sec, "group");
        e.pamtHash = uint32_t(ini.getU64(sec, "pamt_hash"));
        e.pazIndex = uint32_t(ini.getU64(sec, "paz_index", 0xFFFFFFFFu));
        e.pazSize = ini.getU64(sec, "paz_size");
        e.offset = ini.getU64(sec, "offset", UINT64_MAX);
        e.storedSize = uint32_t(ini.getU64(sec, "stored_size"));
        e.size = uint32_t(ini.getU64(sec, "size"));
        e.flags = uint32_t(ini.getU64(sec, "flags", 0xFFFFFFFFu));
        e.origHash = uint32_t(ini.getU64(sec, "orig_hash"));
        e.origHash0 = uint32_t(ini.getU64(sec, "orig_hash0"));
        e.dataFile = ini.get(sec, "data");
        e.dataHash = uint32_t(ini.getU64(sec, "data_hash"));
        e.dataHash0 = uint32_t(ini.getU64(sec, "data_hash0"));
        e.description = ini.get(sec, "description");

        std::string why;
        if (e.path.empty()) why = "missing path";
        else if (e.group.size() != 4) why = "bad group";
        else if (e.pazIndex == 0xFFFFFFFFu) why = "missing paz_index";
        else if (e.offset == UINT64_MAX) why = "missing offset";
        else if (e.storedSize == 0) why = "missing stored_size";
        else if (e.flags == 0xFFFFFFFFu) why = "missing flags";
        else if (e.dataFile.empty()) why = "missing data";
        if (!why.empty()) {
            errors.push_back("[" + sec + "] " + why);
            continue;
        }
        m.entries.push_back(e);
    }
    return m;
}

}  // namespace qs
