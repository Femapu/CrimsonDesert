// QuattroStagioni - test DLL wrapping the real probe code for the fake game (tests/probe_fake/run.sh).
// Exports start/stop with explicit paths and an in-process self test of the module scanner, the
// RTTI resolver and the hook guards against the fake game's sections.
#include <windows.h>

#include <cstdio>
#include <string>

#include "../../src/core/probe/rtti.h"
#include "../../src/win/probe/hooks.h"
#include "../../src/win/probe/module.h"
#include "../../src/win/probe/probe.h"

using namespace qs::probe;

extern "C" __declspec(dllexport) bool qsTestStart(const wchar_t* ini, const wchar_t* log) {
    return qs::probeStartWith(ini, log);
}

extern "C" __declspec(dllexport) void qsTestStop() { qs::probeStop(); }

extern "C" __declspec(dllexport) int qsTestSelfTest(void* prot, void* stub, void* normal, char* out, int outLen) {
    std::string s;
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        s += ok ? "  selftest ok   " : "  selftest FAIL ";
        s += what;
        s += "\n";
        if (!ok) ++fails;
    };
    ModuleInfo m;
    std::string err;
    check(describeModule(nullptr, m, &err), "describeModule");
    const Section* code = m.view.primaryCode();
    check(code && code->name == ".text", "primary code section is .text");
    const Section* p = m.view.sectionOf(m.view.toRva(reinterpret_cast<uintptr_t>(prot)));
    check(p && p->kind == SectionKind::Obfuscated && !p->accessible, ".prot classified obfuscated and not readable");
    uint64_t dest = 0;
    check(checkHookTarget(m, reinterpret_cast<uintptr_t>(prot)) == TargetCheck::Obfuscated, "hook inside .prot refused");
    check(checkHookTarget(m, reinterpret_cast<uintptr_t>(stub), &dest) == TargetCheck::OkJmpStub &&
              dest == m.view.toRva(reinterpret_cast<uintptr_t>(prot)),
          "jmp stub into .prot recognised");
    check(checkHookTarget(m, reinterpret_cast<uintptr_t>(normal)) == TargetCheck::Ok, "plain code target accepted");
    check(checkHookTarget(m, reinterpret_cast<uintptr_t>(&qsTestSelfTest)) == TargetCheck::Outside, "other module refused");
    GuardedMinHook mh;
    void* orig = nullptr;
    std::string why;
    check(!mh.install(m, reinterpret_cast<uintptr_t>(prot), normal, &orig, &why), "MinHook on .prot refused");
    RttiIndex idx;
    idx.build(m.view);
    RttiClass c;
    // slot 1 of TimeOfDay is swapped by the capture probe while this runs, so the walk stops after slot 0
    check(idx.find("pa::TimeOfDay", c) && c.primaryVtable() && idx.vtableSlots(c.primaryVtable()) == 1,
          "RTTI pa::TimeOfDay (slot 1 swapped: walk stops at our stub)");
    check(idx.find("pa::ManagerBase", c) && c.mangled == ".?AUManagerBase@pa@@", "RTTI struct fallback");
    check(idx.find("pa::PAWeatherManager", c) && idx.vtableSlots(c.primaryVtable()) == 3, "RTTI pa::PAWeatherManager, 3 slots");
    std::vector<BaseClass> b = idx.baseClasses(c.cols[0]);
    check(b.size() == 2 && b[1].mangled == ".?AUManagerBase@pa@@", "hierarchy PAWeatherManager : ManagerBase");
    check(idx.find("pa::scriptComponent::weatherTimeSimulatorScriptComponent", c), "nested namespace class");
    b = idx.baseClasses(c.cols[0]);
    check(b.size() == 2 && b[1].mangled == ".?AVTimeOfDay@pa@@", "hierarchy sim : TimeOfDay");
    check(idx.findGlob("pa::*").size() == 4, "glob pa::* finds 4 classes");
    char head[64];
    std::snprintf(head, sizeof(head), "selftest: %d failure(s)\n", fails);
    s = head + s;
    if (out && outLen > 0) {
        std::snprintf(out, size_t(outLen), "%s", s.c_str());
    }
    return fails;
}
