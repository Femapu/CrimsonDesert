// QuattroStagioni - inventory verification. See verify.h.
#include "verify.h"

#include <map>

namespace qs::gen {
namespace {

// Inventory format name -> DdsInfo::format ("rgb32bpp" -> "RGB32").
std::string ddsFormatName(const std::string& fmt) {
    if (fmt.size() > 6 && fmt.compare(0, 3, "rgb") == 0 && fmt.compare(fmt.size() - 3, 3, "bpp") == 0)
        return "RGB" + fmt.substr(3, fmt.size() - 6);
    return fmt;
}

}  // namespace

VerifyReport verifyInventory(GameArchive& archive, const std::vector<Item>& items, bool readData,
                             const std::function<void(size_t, size_t)>& progress) {
    VerifyReport rep;
    // DLC groups, for the shadowing check.
    std::vector<const Group*> dlc;
    for (const std::string& g : archive.groupNames()) {
        if (g.compare(0, 4, "dlc/") != 0) continue;
        std::string err;
        const Group* p = archive.group(g, &err);
        if (p) dlc.push_back(p);
        else { rep.groupErrors++; rep.issues.push_back({g, "", "group", err}); }
    }
    std::map<std::string, bool> badGroup;
    std::vector<uint8_t> stored, plain;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (progress) progress(i, items.size());
        rep.checked++;
        std::string err;
        const Group* g = archive.group(it.group, &err);
        if (!g) {
            if (!badGroup[it.group]) { rep.groupErrors++; rep.issues.push_back({it.group, "", "group", err}); }
            badGroup[it.group] = true;
            rep.missing++;
            continue;
        }
        auto e = g->byPath.find(it.path);
        if (e == g->byPath.end()) {
            rep.missing++;
            rep.issues.push_back({it.group, it.path, "missing", "not in " + it.group + "/0.pamt"});
            continue;
        }
        bool good = true;
        std::string d = compareWithIndex(it, e->second);
        if (!d.empty()) {
            rep.indexMismatch++;
            rep.issues.push_back({it.group, it.path, "index", d});
            good = false;
        }
        for (const Group* dg : dlc) {
            if (dg->name == it.group) continue;
            auto s = dg->byPath.find(it.path);
            if (s != dg->byPath.end()) {
                rep.shadowed++;
                rep.issues.push_back({it.group, it.path, "shadowed", "also in " + dg->name + " (paz " +
                                      std::to_string(s->second.pazIndex) + " @" + std::to_string(s->second.offset) + ")"});
            }
        }
        if (readData && good) {
            EntryLayout L;
            if (!archive.readStored(it.group, e->second, stored, &err) ||
                !decodeStored(e->second, g->keyMaterial, stored.data(), stored.size(), plain, &L, &err)) {
                rep.decodeErrors++;
                rep.issues.push_back({it.group, it.path, "decode", err});
                continue;
            }
            rep.bytesRead += stored.size();
            DdsInfo info;
            std::string hd;
            if (!parseDdsHeader(plain.data(), plain.size(), info, &err)) {
                hd = err;
            } else {
                auto cmp = [&](const char* what, const std::string& inv, const std::string& live) {
                    if (inv == live) return;
                    if (!hd.empty()) hd += ", ";
                    hd += std::string(what) + " " + inv + " -> " + live;
                };
                cmp("fmt", ddsFormatName(it.fmt), info.format);
                cmp("w", std::to_string(it.w), std::to_string(info.width));
                cmp("h", std::to_string(it.h), std::to_string(info.height));
                cmp("mips", std::to_string(it.mips), std::to_string(info.mipCount));
                if (L.compression == 1)
                    for (uint32_t m = 0; m < 4 && m < info.mipCount; ++m)
                        cmp(("mip_stored[" + std::to_string(m) + "]").c_str(), std::to_string(it.mipStored[m]),
                            std::to_string(info.storedMip[m]));
            }
            if (!hd.empty()) {
                rep.headerMismatch++;
                rep.issues.push_back({it.group, it.path, "header", hd});
                good = false;
            }
        }
        if (good) rep.ok++;
    }
    if (progress) progress(items.size(), items.size());
    return rep;
}

}  // namespace qs::gen
