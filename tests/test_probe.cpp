// QuattroStagioni - native unit tests for the portable probe logic (src/core/probe/).
// Usage: test_probe [data_dir]   (data_dir unused: everything is synthetic)
// Covers: pattern parsing/scanning and RIP helpers, section classification, the RTTI resolver on a
// synthetic mapped PE image (TypeDescriptor -> COL -> vtable, hierarchy, globs), probes.ini parsing,
// change detection / field statistics and the capture stub encoding.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/probe/capture_stub.h"
#include "../src/core/probe/diff.h"
#include "../src/core/probe/pattern.h"
#include "../src/core/probe/pe_view.h"
#include "../src/core/probe/probe_config.h"
#include "../src/core/probe/rtti.h"

using namespace qs::probe;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)

template <class T>
static void put(std::vector<uint8_t>& b, size_t off, T v) { std::memcpy(b.data() + off, &v, sizeof(T)); }
static void putStr(std::vector<uint8_t>& b, size_t off, const char* s) { std::memcpy(b.data() + off, s, std::strlen(s) + 1); }

// ---------------------------------------------------------------------------------------------
static void testPattern() {
    Pattern p;
    std::string err;
    CHECK(parsePattern("48 8B 0D ?? ?? ?? ?? 48 85 C9", p, &err));
    CHECK(p.size() == 10);
    CHECK(p.mask[3] == 0 && p.mask[7] == 1);
    CHECK(p.anchorOffset == 0 && p.anchorLength == 3);
    CHECK(formatPattern(p) == "48 8B 0D ?? ?? ?? ?? 48 85 C9");
    Pattern q;
    CHECK(parsePattern("488B0D????????4885C9", q));
    CHECK(formatPattern(q) == formatPattern(p));
    CHECK(parsePattern("? 11 22 33 44 ?", q) && q.anchorOffset == 1 && q.anchorLength == 4);
    CHECK(!parsePattern("?? ??", q, &err));
    CHECK(!parsePattern("4G", q));
    CHECK(!parsePattern("", q));
    CHECK(!parsePattern("123", q));

    std::vector<uint8_t> buf(4096, 0x90);
    const uint8_t sig[] = {0x48, 0x8B, 0x0D, 0x11, 0x22, 0x33, 0x44, 0x48, 0x85, 0xC9};
    std::memcpy(&buf[100], sig, sizeof(sig));
    std::memcpy(&buf[4096 - 10], sig, sizeof(sig));  // at the very end
    buf[2000] = 0x48; buf[2001] = 0x8B; buf[2002] = 0x0D;  // anchor only, no full match
    std::vector<size_t> h = findAll(buf.data(), buf.size(), p);
    CHECK(h.size() == 2 && h[0] == 100 && h[1] == 4086);
    CHECK(findFirst(buf.data(), buf.size(), p) == 100);
    CHECK(findAll(buf.data(), buf.size(), p, 1).size() == 1);
    CHECK(findAll(buf.data(), 9, p).empty());
    // pattern whose anchor is not at the start
    CHECK(parsePattern("?? ?? 8B 0D", q));
    h = findAll(buf.data(), buf.size(), q);
    CHECK(h.size() == 3 && h[0] == 99);

    CHECK(ripTarget(0x1000, 7, 0x10) == 0x1017);
    CHECK(ripTarget(0x1000, 5, -0x20) == 0xFE5);
    CHECK(readI32(sig + 3) == 0x44332211);
    RipSpec r;
    CHECK(parseRipSpec("rip:3:7", r) && r.enabled && r.dispOffset == 3 && r.insnLen == 7);
    CHECK(parseRipSpec("call", r) && r.dispOffset == 1 && r.insnLen == 5);
    CHECK(parseRipSpec("", r) && !r.enabled);
    CHECK(!parseRipSpec("rip:3:5", r));
    CHECK(!parseRipSpec("rop:3:7", r));
}

// ---------------------------------------------------------------------------------------------
static void testClassify() {
    std::vector<Section> s(5);
    s[0].rva = 0x1000; s[0].vsize = 0x1000; s[0].characteristics = kScnMemExecute | kScnMemRead;
    s[1].rva = 0x2000; s[1].vsize = 0x1000; s[1].characteristics = kScnMemRead;
    s[2].rva = 0x3000; s[2].vsize = 0x1000; s[2].characteristics = kScnMemRead | kScnMemWrite;
    s[3].rva = 0x4000; s[3].vsize = 0x8000; s[3].characteristics = kScnMemExecute | kScnMemRead | kScnMemWrite;  // RWX
    s[4].rva = 0xC000; s[4].vsize = 0x1000; s[4].characteristics = kScnMemExecute | kScnMemRead;
    classifySections(s, 0x5000);  // entry in the RWX protector section
    CHECK(s[0].kind == SectionKind::Code);
    CHECK(s[1].kind == SectionKind::ReadOnlyData);
    CHECK(s[2].kind == SectionKind::Data);
    CHECK(s[3].kind == SectionKind::Obfuscated);
    CHECK(s[4].kind == SectionKind::Obfuscated);
    // the entry point does not matter: the lowest non-writable executable section is the game code
    classifySections(s, 0x1800);
    CHECK(s[0].kind == SectionKind::Code && s[4].kind == SectionKind::Obfuscated);
    // a writable executable section is never the game code, even at the lowest RVA
    s[0].characteristics |= kScnMemWrite;
    classifySections(s, 0x1800);
    CHECK(s[0].kind == SectionKind::Obfuscated && s[4].kind == SectionKind::Code);
    // single code section with the entry point (normal exe): it is the game code
    std::vector<Section> one(1);
    one[0].rva = 0x1000; one[0].vsize = 0x100; one[0].characteristics = kScnCntCode | kScnMemExecute | kScnMemRead;
    classifySections(one, 0x1010);
    CHECK(one[0].kind == SectionKind::Code);
}

// ---------------------------------------------------------------------------------------------
// Synthetic mapped image: .text 0x1000 (R-X), .rdata 0x3000 (R--), .data 0x5000 (RW-), .prot 0x7000 (R-X, entry)
static const uint64_t kBase = 0x140000000ull;

static std::vector<uint8_t> makeImage() {
    std::vector<uint8_t> b(0x8000, 0);
    b[0] = 'M'; b[1] = 'Z';
    put<uint32_t>(b, 0x3C, 0x80);
    std::memcpy(&b[0x80], "PE\0\0", 4);
    put<uint16_t>(b, 0x84, 0x8664);
    put<uint16_t>(b, 0x86, 4);
    put<uint32_t>(b, 0x88, 0x6A0B1C2Du);
    put<uint16_t>(b, 0x94, 0xF0);
    const size_t oh = 0x98;
    put<uint16_t>(b, oh, 0x20B);
    put<uint32_t>(b, oh + 16, 0x7000);
    put<uint64_t>(b, oh + 24, kBase);
    put<uint32_t>(b, oh + 56, 0x8000);
    put<uint32_t>(b, oh + 108, 16);
    struct { const char* n; uint32_t rva, ch; } secs[] = {
        {".text", 0x1000, kScnCntCode | kScnMemExecute | kScnMemRead},
        {".rdata", 0x3000, kScnMemRead},
        {".data", 0x5000, kScnMemRead | kScnMemWrite},
        {".prot", 0x7000, kScnMemExecute | kScnMemRead},
    };
    size_t sh = oh + 0xF0;
    for (auto& s : secs) {
        std::memcpy(&b[sh], s.n, std::strlen(s.n));
        put<uint32_t>(b, sh + 8, 0x1000);
        put<uint32_t>(b, sh + 12, s.rva);
        put<uint32_t>(b, sh + 16, 0x1000);
        put<uint32_t>(b, sh + 36, s.ch);
        sh += 40;
    }
    // TypeDescriptors in .data
    put<uint64_t>(b, 0x5000, kBase + 0x3F00);
    putStr(b, 0x5010, ".?AVWeatherManager@pa@@");
    put<uint64_t>(b, 0x5040, kBase + 0x3F00);
    putStr(b, 0x5050, ".?AVManagerBase@pa@@");
    put<uint64_t>(b, 0x5080, kBase + 0x3F00);
    putStr(b, 0x5090, ".?AUTemperatureArea@pa@@");
    // COLs in .rdata
    auto col = [&](uint32_t at, uint32_t td, uint32_t chd) {
        put<uint32_t>(b, at, 1); put<uint32_t>(b, at + 4, 0); put<uint32_t>(b, at + 8, 0);
        put<uint32_t>(b, at + 12, td); put<uint32_t>(b, at + 16, chd); put<uint32_t>(b, at + 20, at);
    };
    col(0x3000, 0x5000, 0x3100);
    col(0x3018, 0x5040, 0x3180);
    col(0x3030, 0x5080, 0x31C0);
    // hierarchy: WeatherManager : ManagerBase
    put<uint32_t>(b, 0x3108, 2); put<uint32_t>(b, 0x310C, 0x3120);
    put<uint32_t>(b, 0x3120, 0x3140); put<uint32_t>(b, 0x3124, 0x3160);
    put<uint32_t>(b, 0x3140, 0x5000); put<uint32_t>(b, 0x3144, 1);
    put<uint32_t>(b, 0x3160, 0x5040); put<uint32_t>(b, 0x3164, 0);
    put<uint32_t>(b, 0x3188, 1); put<uint32_t>(b, 0x318C, 0x31A0); put<uint32_t>(b, 0x31A0, 0x3160);
    put<uint32_t>(b, 0x31C8, 1); put<uint32_t>(b, 0x31CC, 0x31E0); put<uint32_t>(b, 0x31E0, 0x31F0);
    put<uint32_t>(b, 0x31F0, 0x5080);
    // vtables
    put<uint64_t>(b, 0x3200, kBase + 0x3000);
    put<uint64_t>(b, 0x3208, kBase + 0x1000);
    put<uint64_t>(b, 0x3210, kBase + 0x1010);
    put<uint64_t>(b, 0x3218, kBase + 0x1200);  // jmp stub into .prot
    put<uint64_t>(b, 0x3240, kBase + 0x3018);
    put<uint64_t>(b, 0x3248, kBase + 0x1000);
    put<uint64_t>(b, 0x3250, kBase + 0x7000);  // slot pointing straight into the protector
    put<uint64_t>(b, 0x3280, kBase + 0x3030);
    put<uint64_t>(b, 0x3288, kBase + 0x1020);
    // a stray COL reference that is not a vtable (next qword not code)
    put<uint64_t>(b, 0x32C0, kBase + 0x3000);
    put<uint64_t>(b, 0x32C8, 0);
    // code: rip-relative load of a global at 0x5100 from 0x1100; jmp stub at 0x1200 -> 0x7010
    const uint8_t mov[] = {0x48, 0x8B, 0x0D};
    std::memcpy(&b[0x1100], mov, 3);
    put<int32_t>(b, 0x1103, int32_t(0x5100 - (0x1100 + 7)));
    b[0x1200] = 0xE9;
    put<int32_t>(b, 0x1201, int32_t(0x7010 - (0x1200 + 5)));
    return b;
}

static void testRtti() {
    CHECK(mangleClassName("pa::scriptComponent::WeatherSimulatorScriptComponent") ==
          ".?AVWeatherSimulatorScriptComponent@scriptComponent@pa@@");
    CHECK(mangleClassName("Foo") == ".?AVFoo@@");
    CHECK(mangleClassName("pa::TemperatureArea", true) == ".?AUTemperatureArea@pa@@");
    CHECK(mangleClassName(".?AVX@@") == ".?AVX@@");
    CHECK(demangleClassName(".?AVWeatherSimulatorScriptComponent@scriptComponent@pa@@") ==
          "pa::scriptComponent::WeatherSimulatorScriptComponent");
    CHECK(demangleClassName(".?AV?$Vec@H@pa@@") == ".?AV?$Vec@H@pa@@");
    CHECK(globMatch("*weather*", "pa::ServerWeatherManager"));
    CHECK(globMatch("pa::*Manager", "pa::ServerWeatherManager"));
    CHECK(!globMatch("pa::*Manager", "pa::ServerWeatherManagerX"));
    CHECK(globMatch(".?AV*@pa@@", ".?AVFoo@pa@@"));

    std::vector<uint8_t> img = makeImage();
    ImageView v;
    std::string err;
    CHECK(parseMappedImage(img.data(), img.size(), kBase, v, &err));
    CHECK(v.sections.size() == 4 && v.entryRva == 0x7000 && v.timeDateStamp == 0x6A0B1C2Du);
    CHECK(v.sections[0].kind == SectionKind::Code && v.sections[3].kind == SectionKind::Obfuscated);
    CHECK(v.primaryCode() && v.primaryCode()->rva == 0x1000);
    CHECK(v.kindAt(0x5010) == SectionKind::Data && v.kindAt(0x3000) == SectionKind::ReadOnlyData);
    CHECK(v.cstr(0x5010) == ".?AVWeatherManager@pa@@");
    CHECK(v.describe(0x1234).find("code .text+0x234") != std::string::npos);
    uint64_t dest = 0;
    CHECK(decodeJump(v, 0x1200, dest) && dest == 0x7010);
    CHECK(!decodeJump(v, 0x1100, dest));
    // rip helper over the image
    Pattern p;
    CHECK(parsePattern("48 8B 0D ?? ?? ?? ??", p));
    const Section* code = v.primaryCode();
    std::vector<size_t> h = findAll(v.bytes + code->rva, code->vsize, p);
    CHECK(h.size() == 1);
    uint64_t insn = code->rva + h[0];
    CHECK(ripTarget(insn, 7, readI32(v.bytes + insn + 3)) == 0x5100);

    RttiIndex idx;
    CHECK(idx.build(v) == 3);
    CHECK(idx.classCount() == 3);
    RttiClass c;
    CHECK(idx.find("pa::WeatherManager", c));
    CHECK(c.tdRva == 0x5000 && c.cols.size() == 1 && c.primaryVtable() == 0x3208);
    CHECK(c.cols[0].vtableRefs == 1);  // the stray reference at 0x32C0 is ignored
    CHECK(idx.vtableSlots(0x3208) == 3);
    CHECK(idx.vtableSlots(0x3248) == 2);  // .prot is executable too
    CHECK(idx.colForVtable(0x3208) && idx.colForVtable(0x3208)->tdRva == 0x5000);
    CHECK(!idx.colForVtable(0x3210));
    std::vector<BaseClass> bases = idx.baseClasses(c.cols[0]);
    CHECK(bases.size() == 2 && bases[0].mangled == ".?AVWeatherManager@pa@@" && bases[1].mangled == ".?AVManagerBase@pa@@");
    CHECK(idx.find(".?AVManagerBase@pa@@", c) && c.primaryVtable() == 0x3248);
    CHECK(idx.find("pa::TemperatureArea", c) && c.mangled == ".?AUTemperatureArea@pa@@");  // struct fallback
    CHECK(!idx.find("pa::Nope", c));
    std::vector<RttiClass> g = idx.findGlob("*Manager*");
    CHECK(g.size() == 2);
    CHECK(idx.findGlob("*", 1).size() == 1);
    CHECK(idx.typeName(0x5040) == ".?AVManagerBase@pa@@");

    // an inaccessible section is skipped entirely
    v.sections[2].accessible = false;
    RttiIndex idx2;
    CHECK(idx2.build(v) == 0);
    CHECK(!v.readable(0x5010, 4));
}

// ---------------------------------------------------------------------------------------------
static void testConfig() {
    const char* text =
        "[General]\nEnabled=1\nStartDelayMs=1000\nIntervalMs=250\nAllowHooks=1\nExpectTimestamp=0x6A0B1C2D\n"
        "[probe.Weather]\nClass=pa::ServerWeatherManager\nLogVtable=1\nLogHierarchy=yes\nMaxSlots=8\n"
        "Instance=scan\nDump=0x0:0x200\nStats=1\nStatsRanges=0:24, -60:60\nWatch=0x34:f32:climateTemp, 0x31:u8 ; inline comment\n"
        "[probe.Clock]\nSignature=C5 FC 10 05 ?? ?? ?? ?? EB 08\nSigOffset=8\nResolve=rip:4:8\nInstance=global\nChain=0x0, 0x60\n"
        "Watch=0x4:u32:hour\nIntervalMs=1000\n"
        "[probe.Tod]\nClass=.?AVTimeOfDay@pa@@\nInstance=capture\nSlot=33\nCaptureArg=rdx\nMaxInstances=200\n"
        "[probe.List]\nListClasses=*Weather*\n"
        "[probe.Off]\nEnabled=0\nClass=x\nInstance=bogus\n"
        "[probe.Bad1]\nClass=a\nSignature=48\n"
        "[probe.Bad2]\nSignature=48 ?? ZZ\n"
        "[probe.Bad3]\nClass=a\nInstance=capture\n"
        "[probe.Bad4]\nRva=0x1000\nDump=0:0x10\n"
        "[probe.Bad5]\nSignature=48 8B\nInstance=scan\n"
        "[probe.Fn]\nRva=0x40B71E0\nInstance=capture\nHook=minhook\nWatch=0x44:f32\n";
    ProbeConfig cfg;
    std::vector<std::string> errors;
    CHECK(parseProbeConfigText(text, cfg, errors));
    CHECK(cfg.enabled && cfg.startDelayMs == 1000 && cfg.intervalMs == 250 && cfg.allowHooks);
    CHECK(cfg.expectTimestamp == 0x6A0B1C2Du);
    CHECK(cfg.probes.size() == 5);
    CHECK(errors.size() == 5);
    if (cfg.probes.size() == 5) {
        const ProbeSpec& w = cfg.probes[0];
        CHECK(w.name == "weather" && w.className == "pa::ServerWeatherManager" && w.logVtable && w.logHierarchy);
        CHECK(w.maxSlots == 8 && w.instance == InstanceMode::Scan && w.dumpOffset == 0 && w.dumpSize == 0x200);
        CHECK(w.stats && w.statsRanges.size() == 2 && w.statsRanges[1].first == -60.0f);
        CHECK(w.watches.size() == 2 && w.watches[0].offset == 0x34 && w.watches[0].type == FieldType::F32 &&
              w.watches[0].name == "climateTemp" && w.watches[1].type == FieldType::U8);
        const ProbeSpec& k = cfg.probes[1];
        CHECK(k.signature.size() == 10 && k.sigOffset == 8 && k.resolve.enabled && k.resolve.dispOffset == 4);
        CHECK(k.instance == InstanceMode::Global && k.chain.size() == 2 && k.chain[1] == 0x60 && k.intervalMs == 1000);
        const ProbeSpec& t = cfg.probes[2];
        CHECK(t.instance == InstanceMode::Capture && t.slot == 33 && t.captureRdx && t.maxInstances == 64);
        CHECK(cfg.probes[3].listGlob == "*Weather*");
        CHECK(cfg.probes[4].rva == 0x40B71E0 && cfg.probes[4].hook == HookKind::MinHook);
    }
    ProbeConfig c2;
    errors.clear();
    CHECK(!parseProbeConfigText("[probe.x]\nClass=a\n", c2, errors));
    CHECK(parseProbeConfigText("[General]\n", c2, errors) && !c2.enabled && !c2.allowHooks);

    std::vector<int64_t> l;
    CHECK(parseIntList("0x10, -8,3", l) && l.size() == 3 && l[1] == -8);
    CHECK(!parseIntList("0x10,zz", l));
    uint32_t o, s;
    CHECK(parseOffsetSize("0x40:0x100", o, s) && o == 0x40 && s == 0x100);
    CHECK(!parseOffsetSize("0x40", o, s) && !parseOffsetSize("0:0", o, s));
    std::vector<WatchField> wf;
    CHECK(parseWatchList("0x8", wf, nullptr) && wf.size() == 1 && wf[0].type == FieldType::F32);
    CHECK(!parseWatchList("0x8:f99", wf, nullptr));
}

// ---------------------------------------------------------------------------------------------
static void testDiff() {
    uint8_t a[32] = {}, b[32] = {};
    CHECK(diffBytes(a, b, 32).empty());
    b[3] = 1; b[5] = 2; b[20] = 3;
    std::vector<ChangedRange> r = diffBytes(a, b, 32, 4);
    CHECK(r.size() == 2 && r[0].offset == 3 && r[0].length == 3 && r[1].offset == 20 && r[1].length == 1);
    r = diffBytes(a, b, 32, 0);
    CHECK(r.size() == 3);
    r = alignRanges(diffBytes(a, b, 32, 4), 32);
    CHECK(r.size() == 2 && r[0].offset == 0 && r[0].length == 8 && r[1].offset == 20 && r[1].length == 4);
    b[31] = 9;
    r = alignRanges(diffBytes(a, b, 32, 0), 32);
    CHECK(r.back().offset == 28 && r.back().length == 4);

    float f1 = 12.5f, f2 = 12.50001f, f3 = 13.0f;
    uint8_t x[4], y[4], z[4];
    std::memcpy(x, &f1, 4); std::memcpy(y, &f2, 4); std::memcpy(z, &f3, 4);
    CHECK(!fieldChanged(FieldType::F32, x, y, 1e-3));
    CHECK(fieldChanged(FieldType::F32, x, z, 1e-3));
    CHECK(fieldChanged(FieldType::U32, x, y, 1e-3));
    CHECK(formatField(FieldType::F32, x) == "12.5");
    CHECK(formatField(FieldType::U8, z) == "0");
    CHECK(fieldSize(FieldType::Ptr) == 8 && fieldSize(FieldType::I16) == 2);
    FieldType t;
    CHECK(parseFieldType("FLOAT", t) && t == FieldType::F32 && !parseFieldType("f16", t));
    CHECK(hexBytes(x, 2) == "00 00");
    CHECK(describeDwords(0x34, x, 4).find("+0x34") != std::string::npos);
    CHECK(describeDwords(0x34, x, 4).find("f32 12.5") != std::string::npos);

    // stats: offset 0 = hour sweeping 0..24, offset 4 = 0..1 ratio, offset 8 = constant, offset 12 = pointer-like
    FieldStats st;
    st.reset(16);
    for (int i = 0; i < 50; ++i) {
        uint8_t snap[16];
        float hour = std::fmod(i * 0.7f, 24.0f), ratio = i / 49.0f, k = 5.0f;
        uint32_t junk = 0xFFFFFFFFu - uint32_t(i);  // NaN pattern as float
        std::memcpy(snap, &hour, 4); std::memcpy(snap + 4, &ratio, 4); std::memcpy(snap + 8, &k, 4); std::memcpy(snap + 12, &junk, 4);
        st.add(snap);
    }
    CHECK(st.samples() == 50);
    std::vector<FieldStats::Candidate> c24 = st.floatsInRange(0, 24, 10);
    CHECK(c24.size() == 2 && c24[0].offset == 0 && c24[1].offset == 4);
    std::vector<FieldStats::Candidate> c1 = st.floatsInRange(0, 1, 10);
    CHECK(c1.size() == 1 && c1[0].offset == 4);
    std::vector<FieldStats::Candidate> act = st.mostActive(10, 1);
    CHECK(act.size() == 3);  // constant excluded, NaN dword counted as active integer
}

// ---------------------------------------------------------------------------------------------
static void testStub() {
    CaptureStub s = buildCaptureStub(0x1122334455667788ull, 0x0102030405060708ull, 0xAABBCCDDEEFF0011ull);
    CHECK(!s.code.empty() && s.code[0] == 0x51);
    CHECK(s.originalSlotOffset + 8 == s.code.size());
    CHECK(readU64(s.code.data() + s.originalSlotOffset) == 0xAABBCCDDEEFF0011ull);
    // jmp [rip+0] right before the slot
    CHECK(s.code[s.originalSlotOffset - 6] == 0xFF && s.code[s.originalSlotOffset - 5] == 0x25);
    // imm64 operands present
    bool ctxFound = false, cbFound = false;
    for (size_t i = 0; i + 10 <= s.code.size(); ++i) {
        if (s.code[i] == 0x48 && s.code[i + 1] == 0xB9 && readU64(&s.code[i + 2]) == 0x1122334455667788ull) ctxFound = true;
        if (s.code[i] == 0x48 && s.code[i + 1] == 0xB8 && readU64(&s.code[i + 2]) == 0x0102030405060708ull) cbFound = true;
    }
    CHECK(ctxFound && cbFound);
    // push/pop balance: 7 pushes at the start, 7 pops before the jmp
    size_t pops = s.originalSlotOffset - 6 - 11;  // pop r11,r10,rax,r9,r8,rdx,rcx = 11 bytes
    const uint8_t popSeq[] = {0x41, 0x5B, 0x41, 0x5A, 0x58, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59};
    CHECK(std::memcmp(&s.code[pops], popSeq, sizeof(popSeq)) == 0);
    CaptureStub e = buildCaptureStub(1, 2, 3, 4096);
    CHECK(e.originalSlotOffset == 4096 && e.code.size() == s.code.size() - 8);
    CHECK(e.code[e.code.size() - 6] == 0xFF && e.code[e.code.size() - 5] == 0x25);
    CHECK(readU32(&e.code[e.code.size() - 4]) + e.code.size() == 4096);  // rip after jmp + disp == slot
    std::vector<uint8_t> copy = s.code;
    setStubOriginal(copy.data(), s, 42);
    CHECK(readU64(copy.data() + s.originalSlotOffset) == 42);
}

int main(int, char**) {
    testPattern();
    testClassify();
    testRtti();
    testConfig();
    testDiff();
    testStub();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
