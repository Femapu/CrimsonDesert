// QuattroStagioni - MSVC x64 RTTI resolver over an ImageView.
//
// Why: the game ships full RTTI (recon C: ~16k classes, vtables untouched by Denuvo), so classes can
// be found by name in every build without byte signatures:
//   TypeDescriptor ".?AVName@ns@@" -> CompleteObjectLocator (COL, x64: RVAs, pSelf == own RVA)
//   -> vtable (the qword just before slot 0 points to the COL).
// The index is built once by scanning the non-executable sections (a few tens of ms in-process).
// Portable: same code runs over the live module and over synthetic images in the unit tests.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pe_view.h"

namespace qs::probe {

// "pa::scriptComponent::WeatherSimulatorScriptComponent" -> ".?AVWeatherSimulatorScriptComponent@scriptComponent@pa@@"
// Already mangled input (".?A...") is returned unchanged. isStruct selects ".?AU".
std::string mangleClassName(const std::string& name, bool isStruct = false);
// Inverse for plain (non-template) names; template or unknown forms are returned unchanged.
std::string demangleClassName(const std::string& mangled);
// Case-insensitive glob with '*' only ('?' is a literal: it is part of mangled names).
bool globMatch(const std::string& pattern, const std::string& text);

struct ColInfo {
    uint32_t rva = 0;
    uint32_t offset = 0;    // offset of this vtable inside the complete object (0 = primary)
    uint32_t cdOffset = 0;
    uint32_t tdRva = 0;     // TypeDescriptor
    uint32_t chdRva = 0;    // ClassHierarchyDescriptor
    uint32_t vtableRva = 0; // 0 if no vtable references this COL
    uint32_t vtableRefs = 0;
};

struct RttiClass {
    std::string mangled;
    std::string pretty;
    uint32_t tdRva = 0;
    std::vector<ColInfo> cols;  // primary (offset 0) first
    uint32_t primaryVtable() const { return cols.empty() ? 0 : cols[0].vtableRva; }
};

struct BaseClass {
    std::string mangled;
    uint32_t mdisp = 0;            // member displacement of the base inside the object
    uint32_t containedBases = 0;
};

class RttiIndex {
public:
    // Returns the number of COLs found. The view must outlive the index.
    size_t build(const ImageView& v);
    bool built() const { return view_ != nullptr; }
    size_t classCount() const { return byTd_.size(); }
    size_t colCount() const { return cols_.size(); }

    // Name may be mangled or "ns::Class" (tries class then struct).
    bool find(const std::string& name, RttiClass& out) const;
    // All classes whose pretty name (or mangled name, if the glob starts with ".?A") matches.
    std::vector<RttiClass> findGlob(const std::string& glob, size_t maxResults = 1000) const;
    // Base classes from the ClassHierarchyDescriptor (the class itself comes first).
    std::vector<BaseClass> baseClasses(const ColInfo& col) const;
    // Number of consecutive slots (up to maxSlots) pointing into executable sections.
    uint32_t vtableSlots(uint32_t vtableRva, uint32_t maxSlots = 2048) const;
    // Reverse lookup: which class owns a vtable (nullptr if unknown).
    const ColInfo* colForVtable(uint32_t vtableRva) const;
    std::string typeName(uint32_t tdRva) const;  // mangled

private:
    RttiClass makeClass(uint32_t tdRva) const;

    const ImageView* view_ = nullptr;
    std::vector<ColInfo> cols_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> byTd_;  // tdRva -> indices in cols_
    std::unordered_map<uint32_t, uint32_t> byVtable_;           // vtableRva -> index in cols_
};

}  // namespace qs::probe
