// QuattroStagioni - inventory loading. See inventory.h.
#include "inventory.h"

#include "../lookup3.h"
#include "../pack/byte_source.h"
#include "json.h"

namespace qs::gen {

bool parseInventory(const std::string& text, Inventory& out, std::vector<std::string>& errors) {
    out = Inventory();
    json::Value doc;
    std::string err;
    if (!json::parse(text, doc, &err)) {
        errors.push_back("classes file: " + err);
        return false;
    }
    out.version = int(doc["version"].asInt());
    out.game = doc["game"].asString();
    const json::Value& entries = doc["entries"];
    if (!entries.isArray()) {
        errors.push_back("classes file: no \"entries\" array");
        return false;
    }
    size_t idx = 0;
    for (const json::Value& e : entries.items()) {
        Item it;
        it.path = normalizePath(e["path"].asString());
        it.group = e["group"].asString();
        if (it.path.empty() || it.group.empty() || !e["paz"].isNumber() || !e["offset"].isNumber() ||
            !e["stored"].isNumber() || !e["size"].isNumber()) {
            errors.push_back("entry " + std::to_string(idx) + ": missing path/group/paz/offset/stored/size");
            ++idx;
            continue;
        }
        it.paz = uint32_t(e["paz"].asInt());
        it.offset = uint64_t(e["offset"].asInt());
        it.stored = uint32_t(e["stored"].asInt());
        it.size = uint32_t(e["size"].asInt());
        it.fmt = e["fmt"].asString();
        it.w = uint32_t(e["w"].asInt());
        it.h = uint32_t(e["h"].asInt());
        it.mips = uint32_t(e["mips"].asInt());
        const json::Value& ms = e["mip_stored"];
        for (size_t i = 0; i < 4 && i < ms.size(); ++i) it.mipStored[i] = uint32_t(ms[i].asInt());
        it.role = e["role"].asString();
        it.cls = e["cls"].asString();
        it.profile = e["profile"].asString();
        it.looks = e["looks"].asString();
        it.region = e["region"].asString();
        it.asset = e["asset"].asString();
        it.tier = int(e["tier"].asInt());
        const std::string& s = it.asset.empty() ? it.path : it.asset;
        it.seed = cdHash(s.data(), s.size());
        out.items.push_back(std::move(it));
        ++idx;
    }
    return true;
}

bool loadInventory(const std::string& path, Inventory& out, std::vector<std::string>& errors) {
    std::vector<uint8_t> raw;
    if (!pack::readWholeFile(path, raw)) {
        errors.push_back("cannot read " + path);
        return false;
    }
    return parseInventory(std::string(raw.begin(), raw.end()), out, errors);
}

std::string compareWithIndex(const Item& it, const PamtEntry& e) {
    std::string d;
    auto diff = [&](const char* what, uint64_t inv, uint64_t live) {
        if (inv == live) return;
        if (!d.empty()) d += ", ";
        d += std::string(what) + " " + std::to_string(inv) + " -> " + std::to_string(live);
    };
    diff("paz", it.paz, e.pazIndex);
    diff("offset", it.offset, e.offset);
    diff("stored", it.stored, e.storedSize);
    diff("size", it.size, e.size);
    return d;
}

}  // namespace qs::gen
