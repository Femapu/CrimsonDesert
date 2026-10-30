// QuattroStagioni - F5 menu: seasonal ImGui style, vector season icons, year wheel and the five tabs.
//
// Everything is drawn with ImGui widgets and draw-list primitives (we cannot ship image assets), and
// nothing here knows about D3D or hooks, so a different front end (a ReShade add-on, the standalone
// demo) can call draw() as is. Config edits go to a local copy of shared().config that is written back
// under the mutex, with configDirty set, only when a widget changed something.
#include "menu.h"

#include "i18n.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "imgui.h"
#include "overlay.h"
#include "qs_state.h"

namespace qs {
using qs::i18n::tr;

namespace {

using season::DaySource;
using season::Phase;
using season::Region;
using season::SeasonConfig;
using season::SeasonState;

constexpr int kRegions = static_cast<int>(Region::Count);
constexpr float kPi = 3.14159265358979f;

MenuSaveFn g_saveFn = nullptr;
std::string g_modVersion = "(versione non impostata)";

ImFont* g_fontBody = nullptr;
ImFont* g_fontBold = nullptr;
float g_scale = 1.0f;
bool g_prewarmed = false;

// Rasterises every (font, size) the menu uses up front: each new glyph later costs a synchronous
// texture upload on the game's queue, i.e. a visible hitch. Sizes are style.FontSizeBase multiples.
void prewarmFonts() {
    if (g_prewarmed) return;
    g_prewarmed = true;
    static const wchar_t extra[] = L"\u00e0\u00e8\u00e9\u00ec\u00f2\u00f9\u00c0\u00c8\u00c9\u00cc\u00d2\u00d9\u00b0\u00b1\u00d7\u00b7";
    const float k = ImGui::GetStyle().FontScaleMain * ImGui::GetStyle().FontScaleDpi;
    const float base = ImGui::GetStyle().FontSizeBase;
    struct Use {
        ImFont* font;
        float mul;
    } uses[] = {{g_fontBody, 1.0f}, {g_fontBody, 0.86f}, {g_fontBold, 1.0f}, {g_fontBold, 1.25f},
                {g_fontBold, 1.6f}, {g_fontBold, 1.75f}, {g_fontBold, 2.6f}};
    for (const Use& u : uses) {
        if (!u.font) continue;
        ImFontBaked* b = u.font->GetFontBaked(base * u.mul * k);
        if (!b) continue;
        for (ImWchar c = 0x20; c < 0x7F; ++c) b->FindGlyph(c);
        for (const wchar_t* p = extra; *p; ++p) b->FindGlyph(ImWchar(*p));
    }
}

// ------------------------------------------------------------------ palette
struct Rgb {
    float r, g, b;
};
const Rgb kPhaseColor[season::kPhaseCount] = {
    {0.56f, 0.80f, 0.38f},  // Primavera: fresh green
    {0.98f, 0.77f, 0.26f},  // Estate: golden sun
    {0.86f, 0.64f, 0.33f},  // Fine estate: straw
    {0.88f, 0.42f, 0.18f},  // Autunno: rust orange
    {0.62f, 0.80f, 0.96f},  // Inverno: ice blue
};
const ImVec4 kBg(0.105f, 0.090f, 0.078f, 0.96f);
const ImVec4 kPanel(0.155f, 0.132f, 0.112f, 1.00f);
const ImVec4 kPanelHi(0.205f, 0.172f, 0.142f, 1.00f);
const ImVec4 kText(0.95f, 0.91f, 0.84f, 1.00f);
const ImVec4 kTextDim(0.66f, 0.60f, 0.52f, 1.00f);
const ImVec4 kAccent(0.90f, 0.58f, 0.24f, 1.00f);
const ImVec4 kAccentHi(0.98f, 0.68f, 0.32f, 1.00f);
const ImVec4 kWarn(0.93f, 0.55f, 0.35f, 1.00f);

ImU32 col(const Rgb& c, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, a)); }
ImU32 col(const ImVec4& c, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
Rgb lerp(const Rgb& a, const Rgb& b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgb scaled(const Rgb& a, float k) { return {a.r * k, a.g * k, a.b * k}; }
float S(float v) { return v * g_scale; }

// ------------------------------------------------------------------ season helpers (local, UI only)
const char* phaseName(int p) {
    const char* n[] = {tr("Primavera"), tr("Estate"), tr("Fine estate"), tr("Autunno"), tr("Inverno")};
    return (p >= 0 && p < season::kPhaseCount) ? n[p] : "?";
}
const char phaseLetter[] = {'S', 'U', 'L', 'A', 'W'};
const char* regionName(int r) {
    const char* n[] = {tr("Temperata"), tr("Nord freddo"), tr("Palude e giungla"), tr("Acquatica"), tr("Deserto"), tr("Abisso")};
    return (r >= 0 && r < kRegions) ? n[r] : "?";
}
const char* regionHint(int r) {
    const char* n[] = {tr("ciclo completo"), tr("inverno lungo e anticipato"), tr("sempreverde, solo umidità"),
                              tr("variazioni minime"), tr("sempre vanilla"), tr("sempre vanilla")};
    return (r >= 0 && r < kRegions) ? n[r] : "";
}
bool excludedByDefault(int r) { return r == int(Region::DesertDry) || r == int(Region::Abyss); }

float yearLength(const SeasonConfig& c) {
    float y = 0;
    for (float d : c.phaseDays) y += std::max(0.0f, d);
    return std::max(1.0f, y);
}
float phaseStart(const SeasonConfig& c, int p) {
    float s = 0;
    for (int i = 0; i < p; ++i) s += std::max(0.0f, c.phaseDays[i]);
    return s;
}
void phaseAt(const SeasonConfig& c, float day, int& phase, float& progress) {
    float y = yearLength(c);
    day = std::fmod(std::fmod(day, y) + y, y);
    float s = 0;
    for (int i = 0; i < season::kPhaseCount; ++i) {
        float len = std::max(0.0f, c.phaseDays[i]);
        if (day < s + len || i == season::kPhaseCount - 1) {
            phase = i;
            progress = len > 0 ? std::min(1.0f, (day - s) / len) : 0.0f;
            return;
        }
        s += len;
    }
    phase = 0;
    progress = 0;
}
std::string stageText(const season::VisualStage& v) {
    char b[32];
    if (v.step == 0) snprintf(b, sizeof b, "%c", phaseLetter[int(v.from) % 5]);
    else snprintf(b, sizeof b, "%c>%c.%d", phaseLetter[int(v.from) % 5], phaseLetter[int(v.to) % 5], v.step);
    return b;
}
void hourText(float h, char* out, size_t n) {
    h = std::fmod(std::fmod(h, 24.0f) + 24.0f, 24.0f);
    int m = int(std::lround(h * 60.0f)) % (24 * 60);
    snprintf(out, n, "%02d:%02d", m / 60, m % 60);
}

// ------------------------------------------------------------------ vector icons
void iconFlower(ImDrawList* dl, ImVec2 c, float r, float a) {
    const Rgb petal{0.99f, 0.78f, 0.86f}, petalEdge{0.93f, 0.55f, 0.70f}, heart{0.99f, 0.84f, 0.30f};
    for (int i = 0; i < 5; ++i) {
        float ang = -kPi / 2 + i * 2 * kPi / 5;
        ImVec2 pc(c.x + std::cos(ang) * r * 0.46f, c.y + std::sin(ang) * r * 0.46f);
        dl->AddEllipseFilled(pc, ImVec2(r * 0.40f, r * 0.27f), col(petalEdge, a), ang);
        dl->AddEllipseFilled(pc, ImVec2(r * 0.34f, r * 0.21f), col(petal, a), ang);
    }
    dl->AddCircleFilled(c, r * 0.24f, col(heart, a));
    dl->AddCircleFilled(ImVec2(c.x - r * 0.06f, c.y - r * 0.06f), r * 0.09f, col(Rgb{1, 0.95f, 0.7f}, a));
}

void iconSun(ImDrawList* dl, ImVec2 c, float r, float a) {
    const Rgb core{0.99f, 0.80f, 0.25f}, ray{0.99f, 0.66f, 0.18f};
    for (int i = 0; i < 8; ++i) {
        float ang = i * kPi / 4;
        float w = 0.13f;
        ImVec2 tip(c.x + std::cos(ang) * r * 0.98f, c.y + std::sin(ang) * r * 0.98f);
        ImVec2 b1(c.x + std::cos(ang - w) * r * 0.56f, c.y + std::sin(ang - w) * r * 0.56f);
        ImVec2 b2(c.x + std::cos(ang + w) * r * 0.56f, c.y + std::sin(ang + w) * r * 0.56f);
        dl->AddTriangleFilled(b1, tip, b2, col(ray, a));
    }
    dl->AddCircleFilled(c, r * 0.50f, col(ray, a));
    dl->AddCircleFilled(c, r * 0.43f, col(core, a));
    dl->AddCircleFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.12f), r * 0.14f, col(Rgb{1, 0.93f, 0.62f}, a * 0.8f));
}

void iconWheat(ImDrawList* dl, ImVec2 c, float r, float a) {
    const Rgb stem{0.72f, 0.55f, 0.28f}, grain{0.93f, 0.72f, 0.36f}, grainHi{0.99f, 0.85f, 0.52f};
    ImVec2 bottom(c.x + r * 0.10f, c.y + r * 0.95f), top(c.x - r * 0.05f, c.y - r * 0.80f);
    dl->AddBezierQuadratic(bottom, ImVec2(c.x + r * 0.18f, c.y), top, col(stem, a), std::max(1.5f, r * 0.09f));
    for (int i = 0; i < 4; ++i) {
        float t = 0.18f + i * 0.17f;
        ImVec2 p(top.x + (bottom.x - top.x) * t * 0.6f, top.y + (bottom.y - top.y) * t);
        float ang = 0.75f;
        dl->AddEllipseFilled(ImVec2(p.x - r * 0.17f, p.y), ImVec2(r * 0.21f, r * 0.10f), col(grain, a), -ang);
        dl->AddEllipseFilled(ImVec2(p.x + r * 0.17f, p.y), ImVec2(r * 0.21f, r * 0.10f), col(grain, a), ang);
    }
    dl->AddEllipseFilled(ImVec2(top.x, top.y - r * 0.02f), ImVec2(r * 0.09f, r * 0.19f), col(grainHi, a));
    // a long awn on the top grain
    dl->AddLine(ImVec2(top.x, top.y - r * 0.18f), ImVec2(top.x - r * 0.06f, top.y - r * 0.32f), col(grainHi, a * 0.8f), std::max(1.0f, r * 0.04f));
}

void iconLeaf(ImDrawList* dl, ImVec2 c, float r, float a) {
    const Rgb fill{0.90f, 0.43f, 0.16f}, fillHi{0.98f, 0.62f, 0.22f}, vein{0.55f, 0.22f, 0.08f};
    // leaf tilted 45 degrees: base bottom-left, tip top-right
    auto rot = [&](float x, float y) {
        const float k = 0.7071f;
        return ImVec2(c.x + (x - y) * k * r, c.y + (x + y) * k * r);
    };
    ImVec2 base = rot(0.0f, 0.78f), tip = rot(0.0f, -0.92f);
    dl->PathLineTo(base);
    dl->PathBezierCubicCurveTo(rot(0.78f, 0.45f), rot(0.70f, -0.50f), tip);
    dl->PathBezierCubicCurveTo(rot(-0.70f, -0.50f), rot(-0.78f, 0.45f), base);
    dl->PathFillConvex(col(fill, a));
    dl->PathLineTo(rot(0.0f, 0.60f));
    dl->PathBezierCubicCurveTo(rot(0.50f, 0.30f), rot(0.48f, -0.40f), rot(0.0f, -0.80f));
    dl->PathLineTo(rot(0.0f, 0.60f));
    dl->PathFillConvex(col(fillHi, a * 0.55f));
    float th = std::max(1.0f, r * 0.06f);
    dl->AddLine(rot(0.0f, 1.0f), rot(0.0f, -0.78f), col(vein, a), th);
    for (int i = 0; i < 3; ++i) {
        float y = 0.35f - i * 0.36f;
        dl->AddLine(rot(0.0f, y), rot(0.36f, y - 0.26f), col(vein, a * 0.85f), th * 0.75f);
        dl->AddLine(rot(0.0f, y), rot(-0.36f, y - 0.26f), col(vein, a * 0.85f), th * 0.75f);
    }
}

void iconSnowflake(ImDrawList* dl, ImVec2 c, float r, float a) {
    const Rgb ice{0.80f, 0.91f, 1.00f}, iceDeep{0.50f, 0.72f, 0.95f};
    float th = std::max(1.2f, r * 0.11f);
    for (int pass = 0; pass < 2; ++pass) {
        ImU32 cc = pass == 0 ? col(iceDeep, a) : col(ice, a);
        float t = pass == 0 ? th * 1.6f : th;
        for (int i = 0; i < 6; ++i) {
            float ang = -kPi / 2 + i * kPi / 3;
            ImVec2 d(std::cos(ang), std::sin(ang)), n(-d.y, d.x);
            ImVec2 tip(c.x + d.x * r * 0.95f, c.y + d.y * r * 0.95f);
            dl->AddLine(c, tip, cc, t);
            for (int k = 0; k < 2; ++k) {
                float at = k == 0 ? 0.45f : 0.70f, len = k == 0 ? 0.30f : 0.22f;
                ImVec2 p(c.x + d.x * r * at, c.y + d.y * r * at);
                dl->AddLine(p, ImVec2(p.x + (d.x + n.x) * r * len * 0.7f, p.y + (d.y + n.y) * r * len * 0.7f), cc, t * 0.85f);
                dl->AddLine(p, ImVec2(p.x + (d.x - n.x) * r * len * 0.7f, p.y + (d.y - n.y) * r * len * 0.7f), cc, t * 0.85f);
            }
        }
    }
    dl->AddCircleFilled(c, th * 1.1f, col(ice, a));
}

void drawSeasonIcon(ImDrawList* dl, ImVec2 c, float r, int phase, float a = 1.0f) {
    switch (phase) {
        case 0: iconFlower(dl, c, r, a); break;
        case 1: iconSun(dl, c, r, a); break;
        case 2: iconWheat(dl, c, r, a); break;
        case 3: iconLeaf(dl, c, r, a); break;
        default: iconSnowflake(dl, c, r, a); break;
    }
}

// Icon inline with text (advances the cursor like a widget).
void inlineIcon(int phase, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    drawSeasonIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.46f, phase);
}

// ------------------------------------------------------------------ custom widgets
bool toggle(const char* label, bool* v, const char* hint = nullptr) {
    ImGuiStyle& st = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight() * 0.82f, w = h * 1.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    float textW = ImGui::CalcTextSize(label, nullptr, true).x;
    bool pressed = ImGui::InvisibleButton("##t", ImVec2(w + st.ItemInnerSpacing.x * 2 + textW, ImGui::GetFrameHeight()));
    if (pressed) *v = !*v;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    ImU32 bg = *v ? col(hov ? kAccentHi : kAccent) : col(hov ? ImVec4(0.32f, 0.28f, 0.24f, 1) : ImVec4(0.26f, 0.23f, 0.20f, 1));
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), bg, h * 0.5f);
    float kx = *v ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, y + h * 0.5f), h * 0.5f - S(2.5f), col(ImVec4(0.98f, 0.95f, 0.90f, 1)));
    const char* end = strstr(label, "##");
    dl->AddText(ImVec2(p.x + w + st.ItemInnerSpacing.x * 2, p.y + st.FramePadding.y), col(kText), label, end);
    if (hint && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", hint);
    return pressed;
}

void dimText(const char* fmt, ...) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextWrappedV(fmt, ap);
    va_end(ap);
    ImGui::PopStyleColor();
}

void sectionTitle(const char* t) {
    ImGui::PushFont(g_fontBold, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentHi);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Horizontal bar with label on the left and value on the right.
void meter(const char* label, float v, const Rgb& c, const char* valueText = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float labelW = S(112);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(w, h + S(4)));
    dl->AddText(p, col(kText), label);
    char buf[32];
    if (!valueText) {
        snprintf(buf, sizeof buf, "%d%%", int(std::lround(std::clamp(v, 0.0f, 1.0f) * 100)));
        valueText = buf;
    }
    float vw = ImGui::CalcTextSize(valueText).x;
    float x0 = p.x + labelW, x1 = p.x + w - vw - S(10);
    float bh = S(8), by = p.y + (h - bh) * 0.5f;
    dl->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), col(ImVec4(0.25f, 0.22f, 0.19f, 1)), bh * 0.5f);
    float f = std::clamp(v, 0.0f, 1.0f);
    if (f > 0.001f)
        dl->AddRectFilledMultiColor(ImVec2(x0, by), ImVec2(x0 + (x1 - x0) * f, by + bh), col(scaled(c, 0.75f)), col(c), col(c),
                                    col(scaled(c, 0.75f)));
    dl->AddText(ImVec2(x1 + S(10), p.y), col(kTextDim), valueText);
}

// Ring segment from angle a0 to a1 with a colour gradient (used by the year wheel).
void ringSegment(ImDrawList* dl, ImVec2 c, float r0, float r1, float a0, float a1, const Rgb& ca, const Rgb& cb) {
    int n = std::max(2, int(std::fabs(a1 - a0) / (kPi / 90)));
    // no AA fringe between the slices (it shows as thin radial seams); the outline is drawn separately
    const ImDrawListFlags saved = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int i = 0; i < n; ++i) {
        float t0 = float(i) / n, t1 = float(i + 1) / n;
        float b0 = a0 + (a1 - a0) * t0, b1 = a0 + (a1 - a0) * t1 + 0.002f;
        ImU32 cc = col(lerp(ca, cb, (t0 + t1) * 0.5f));
        ImVec2 q[4] = {ImVec2(c.x + std::cos(b0) * r0, c.y + std::sin(b0) * r0), ImVec2(c.x + std::cos(b0) * r1, c.y + std::sin(b0) * r1),
                       ImVec2(c.x + std::cos(b1) * r1, c.y + std::sin(b1) * r1), ImVec2(c.x + std::cos(b1) * r0, c.y + std::sin(b1) * r0)};
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], cc);
    }
    dl->Flags = saved;
}

// The year as a wheel: five phase arcs (plateau colour, then a gradient over the transition tail),
// an icon per phase and a needle for today.
void yearWheel(const SeasonConfig& cfg, float today, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    float R = size * 0.5f - S(8), thick = size * 0.16f, r0 = R - thick;
    float year = yearLength(cfg);
    auto angOf = [&](float day) { return -kPi / 2 + 2 * kPi * (day / year); };
    dl->AddCircleFilled(c, R + S(4), col(ImVec4(0.07f, 0.06f, 0.05f, 0.9f)), 96);
    float tf = std::clamp(cfg.transitionFraction, 0.0f, 0.95f);
    for (int i = 0; i < season::kPhaseCount; ++i) {
        float s = phaseStart(cfg, i), len = std::max(0.0f, cfg.phaseDays[i]);
        if (len <= 0) continue;
        float plateauEnd = s + len * (1 - tf);
        const Rgb& a = kPhaseColor[i];
        const Rgb& b = kPhaseColor[(i + 1) % season::kPhaseCount];
        ringSegment(dl, c, r0, R, angOf(s), angOf(plateauEnd), scaled(a, 0.92f), a);
        ringSegment(dl, c, r0, R, angOf(plateauEnd), angOf(s + len), a, b);
        // separator tick and icon at the middle of the plateau
        float sa = angOf(s);
        dl->AddLine(ImVec2(c.x + std::cos(sa) * (r0 - S(3)), c.y + std::sin(sa) * (r0 - S(3))),
                    ImVec2(c.x + std::cos(sa) * (R + S(3)), c.y + std::sin(sa) * (R + S(3))), col(kBg), S(2.5f));
        float ma = angOf(s + len * (1 - tf) * 0.5f);
        float ir = (r0 + R) * 0.5f;
        ImVec2 ic(c.x + std::cos(ma) * ir, c.y + std::sin(ma) * ir);
        dl->AddCircleFilled(ic, thick * 0.40f, col(ImVec4(0.09f, 0.075f, 0.065f, 0.88f)), 32);
        drawSeasonIcon(dl, ic, thick * 0.29f, i);
    }
    dl->AddCircle(c, R, col(ImVec4(0.07f, 0.06f, 0.05f, 1)), 128, S(1.5f));
    dl->AddCircle(c, r0, col(ImVec4(0.07f, 0.06f, 0.05f, 1)), 128, S(1.5f));
    // inner disc
    float inner = r0 - S(10);
    dl->AddCircleFilled(c, inner, col(kPanel), 96);
    dl->AddCircle(c, inner, col(kPanelHi), 96, S(1.5f));
    // needle for today
    float ta = angOf(std::fmod(std::fmod(today, year) + year, year));
    ImVec2 d(std::cos(ta), std::sin(ta));
    ImVec2 outer(c.x + d.x * (R + S(7)), c.y + d.y * (R + S(7)));
    ImVec2 innerP(c.x + d.x * (r0 - S(5)), c.y + d.y * (r0 - S(5)));
    dl->AddLine(innerP, outer, col(ImVec4(0, 0, 0, 0.55f)), S(6));
    dl->AddLine(innerP, outer, col(ImVec4(1, 0.97f, 0.92f, 1)), S(3));
    dl->AddCircleFilled(outer, S(7), col(ImVec4(1, 0.97f, 0.92f, 1)));
    dl->AddCircleFilled(outer, S(4), col(kAccent));
    // centre text
    int ph;
    float prog;
    phaseAt(cfg, today, ph, prog);
    char big[16], small[48];
    snprintf(big, sizeof big, "%d", int(std::floor(std::fmod(std::fmod(today, year) + year, year))) + 1);
    snprintf(small, sizeof small, tr("giorno su %d"), int(std::lround(year)));
    ImGui::PushFont(g_fontBold, ImGui::GetStyle().FontSizeBase * 2.6f);
    ImVec2 bs = ImGui::CalcTextSize(big);
    dl->AddText(ImVec2(c.x - bs.x * 0.5f, c.y - bs.y * 0.78f), col(kText), big);
    ImGui::PopFont();
    ImVec2 ss = ImGui::CalcTextSize(small);
    dl->AddText(ImVec2(c.x - ss.x * 0.5f, c.y + bs.y * 0.22f), col(kTextDim), small);
    const char* pn = phaseName(ph);
    ImVec2 ps = ImGui::CalcTextSize(pn);
    dl->AddText(ImVec2(c.x - ps.x * 0.5f, c.y + bs.y * 0.22f + ss.y + S(2)), col(kPhaseColor[ph]), pn);
}

// Phase lengths as a horizontal strip (Stagioni tab).
void yearStrip(const SeasonConfig& cfg, float today) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(30);
    ImGui::Dummy(ImVec2(w, h + S(6)));
    float year = yearLength(cfg), x = p.x, tf = std::clamp(cfg.transitionFraction, 0.0f, 0.95f);
    for (int i = 0; i < season::kPhaseCount; ++i) {
        float len = std::max(0.0f, cfg.phaseDays[i]) / year * w;
        const Rgb& a = kPhaseColor[i];
        const Rgb& b = kPhaseColor[(i + 1) % season::kPhaseCount];
        float px = x + len * (1 - tf);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(px, p.y + h), col(a), 0);
        dl->AddRectFilledMultiColor(ImVec2(px, p.y), ImVec2(x + len, p.y + h), col(a), col(b), col(b), col(a));
        if (len > S(30)) {
            ImVec2 ic(x + S(17), p.y + h * 0.5f);
            dl->AddCircleFilled(ic, h * 0.40f, col(ImVec4(0.09f, 0.075f, 0.065f, 0.85f)), 24);
            drawSeasonIcon(dl, ic, h * 0.29f, i);
        }
        if (i > 0) dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), col(kBg), S(2));
        x += len;
    }
    float tx = p.x + std::fmod(std::fmod(today, year) + year, year) / year * w;
    dl->AddTriangleFilled(ImVec2(tx - S(6), p.y - S(2)), ImVec2(tx + S(6), p.y - S(2)), ImVec2(tx, p.y + S(7)), col(kText));
    dl->AddLine(ImVec2(tx, p.y), ImVec2(tx, p.y + h), col(kText), S(2));
}

// 24 h bar with daylight between sunrise and sunset.
void dayBar(float sunrise, float sunset) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(14);
    ImGui::Dummy(ImVec2(w, h + S(20)));
    const ImU32 night = col(ImVec4(0.13f, 0.15f, 0.27f, 1)), dawn = col(ImVec4(0.95f, 0.55f, 0.30f, 1)),
                noon = col(ImVec4(0.99f, 0.84f, 0.45f, 1));
    float xr = p.x + w * std::clamp(sunrise / 24.0f, 0.0f, 1.0f), xs = p.x + w * std::clamp(sunset / 24.0f, 0.0f, 1.0f);
    float xm = (xr + xs) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h), night, h * 0.5f);
    if (xs > xr) {
        dl->AddRectFilledMultiColor(ImVec2(xr, p.y), ImVec2(xm, p.y + h), dawn, noon, noon, dawn);
        dl->AddRectFilledMultiColor(ImVec2(xm, p.y), ImVec2(xs, p.y + h), noon, dawn, dawn, noon);
    }
    for (int hr = 0; hr <= 24; hr += 6) {
        float x = p.x + w * hr / 24.0f;
        char b[8];
        snprintf(b, sizeof b, "%d", hr);
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(std::clamp(x - ts.x * 0.5f, p.x, p.x + w - ts.x), p.y + h + S(3)), col(kTextDim), b);
    }
    iconSun(dl, ImVec2(xm, p.y + h * 0.5f), h * 0.85f, 1.0f);
}

// ------------------------------------------------------------------ per-frame state
struct Frame {
    SeasonConfig cfg;
    SeasonState st;
    std::string status;
    bool clockHooked = false;
    bool engineLive = false;  // false until the season engine produced a state
    bool changed = false;
};

float displayDay(const Frame& f) { return f.cfg.daySource == DaySource::Manual ? f.cfg.startDay : f.st.dayOfYear; }

std::string g_saveMsg;
bool g_saveOk = false;
double g_saveTime = -100;
int g_liveRegion = 0;

// ------------------------------------------------------------------ tabs
void tabCalendar(Frame& f) {
    SeasonConfig& c = f.cfg;
    float year = yearLength(c);
    float today = displayDay(f);
    float wheel = S(330);
    ImGui::BeginGroup();
    yearWheel(c, today, wheel);
    ImGui::EndGroup();
    ImGui::SameLine(0, S(26));
    ImGui::BeginGroup();
    int ph;
    float prog;
    phaseAt(c, today, ph, prog);
    sectionTitle(tr("Oggi"));
    inlineIcon(ph, S(34));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(g_fontBold, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextColored(ImVec4(kPhaseColor[ph].r, kPhaseColor[ph].g, kPhaseColor[ph].b, 1), "%s", phaseName(ph));
    ImGui::PopFont();
    dimText(tr("giorno %.1f di %.0f, %d%% della fase"), std::fmod(std::fmod(today, year) + year, year) + 1, year,
            int(std::lround(prog * 100)));
    ImGui::EndGroup();
    meter(tr("Fase"), prog, kPhaseColor[ph]);
    {
        int next = (ph + 1) % season::kPhaseCount;
        float left = std::max(0.0f, c.phaseDays[ph] * (1.0f - prog));
        ImGui::Text(tr("Prossima fase: "));
        ImGui::SameLine(0, 0);
        ImGui::TextColored(ImVec4(kPhaseColor[next].r, kPhaseColor[next].g, kPhaseColor[next].b, 1), "%s", phaseName(next));
        ImGui::SameLine(0, 0);
        ImGui::Text(tr(" tra %.1f giorni"), left);
    }
    if (f.engineLive) {
        ImGui::Text(tr("Stadio visivo (Temperata): %s"), stageText(f.st.stage[0]).c_str());
        dimText(tr("contatore continuo: giorno %.2f"), f.st.day);
    } else {
        dimText(tr("Motore stagioni in attesa: i valori dal vivo compariranno qui."));
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    sectionTitle(tr("Sorgente del giorno"));
    int src = int(c.daySource);
    const char* srcNames[] = {tr("Orologio di gioco"), tr("Tempo di gioco"), tr("Manuale")};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(srcNames[i], src == i)) {
            src = i;
            c.daySource = DaySource(i);
            f.changed = true;
        }
    }
    if (c.daySource == DaySource::GameClock && !f.clockHooked)
        dimText(tr("Orologio del gioco non ancora agganciato: si usa il tempo di gioco finché non è disponibile."));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (c.daySource == DaySource::Playtime) {
        f.changed |= ImGui::SliderFloat("##mpd", &c.realMinutesPerDay, 5.0f, 240.0f, tr("%.0f minuti reali per giorno"),
                                        ImGuiSliderFlags_AlwaysClamp);
    } else if (c.daySource == DaySource::Manual) {
        float shown = c.startDay + 1.0f;  // 1-based like every other day shown in the menu
        if (ImGui::SliderFloat("##day", &shown, 1.0f, year + 0.99f, tr("giorno %.1f"), ImGuiSliderFlags_AlwaysClamp)) {
            c.startDay = shown - 1.0f;
            f.changed = true;
        }
    } else {
        ImGui::BeginDisabled();
        float dummy = c.realMinutesPerDay;
        ImGui::SliderFloat("##mpd2", &dummy, 5.0f, 240.0f, tr("segue l'orologio del gioco"));
        ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    sectionTitle(tr("Vai all'inizio di"));
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5;
    for (int i = 0; i < season::kPhaseCount; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kPhaseColor[i].r * 0.30f, kPhaseColor[i].g * 0.30f, kPhaseColor[i].b * 0.30f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kPhaseColor[i].r * 0.45f, kPhaseColor[i].g * 0.45f, kPhaseColor[i].b * 0.45f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kPhaseColor[i].r * 0.60f, kPhaseColor[i].g * 0.60f, kPhaseColor[i].b * 0.60f, 1));
        if (ImGui::Button("##go", ImVec2(bw, S(58)))) {
            c.daySource = DaySource::Manual;
            c.startDay = phaseStart(c, i);
            f.changed = true;
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip(tr("Passa a modalità manuale, giorno %.0f"), phaseStart(c, i) + 1);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawSeasonIcon(dl, ImVec2(bp.x + bw * 0.5f, bp.y + S(20)), S(12), i);
        const char* n = phaseName(i);
        ImVec2 ts = ImGui::CalcTextSize(n);
        float sc = std::min(1.0f, (bw - S(6)) / ts.x);
        ImFont* fnt = ImGui::GetFont();
        float fs = ImGui::GetFontSize() * 0.86f * sc;
        ImVec2 ts2 = fnt->CalcTextSizeA(fs, FLT_MAX, 0, n);
        dl->AddText(fnt, fs, ImVec2(bp.x + (bw - ts2.x) * 0.5f, bp.y + S(36)), col(kText), n);
        ImGui::PopID();
    }
    ImGui::EndGroup();
}

void tabSeasons(Frame& f) {
    SeasonConfig& c = f.cfg;
    sectionTitle(tr("Lunghezza delle fasi"));
    yearStrip(c, displayDay(f));
    for (int i = 0; i < season::kPhaseCount; ++i) {
        ImGui::PushID(i);
        inlineIcon(i, ImGui::GetFrameHeight());
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(phaseName(i));
        ImGui::SameLine(S(150));
        ImGui::SetNextItemWidth(-FLT_MIN);
        f.changed |= ImGui::SliderFloat("##len", &c.phaseDays[i], 1.0f, 60.0f, tr("%.0f giorni"), ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
    }
    dimText(tr("Un anno dura %.0f giorni di gioco. L'estate è il gioco originale."), yearLength(c));
    ImGui::Dummy(ImVec2(0, S(4)));
    sectionTitle(tr("Transizioni"));
    float pct = c.transitionFraction * 100.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Coda di transizione"));
    ImGui::SameLine(S(210));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##tf", &pct, 0.0f, 90.0f, tr("%.0f%% di ogni fase"), ImGuiSliderFlags_AlwaysClamp)) {
        c.transitionFraction = pct / 100.0f;
        f.changed = true;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Stadi intermedi"));
    ImGui::SameLine(S(210));
    ImGui::SetNextItemWidth(-FLT_MIN);
    f.changed |= ImGui::SliderInt("##steps", &c.transitionSteps, 0, 6, c.transitionSteps == 0 ? tr("cambio netto") : tr("%d stadi"),
                                  ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip(tr("Più stadi: passaggio più graduale, ma più spazio su disco per i pacchetti."));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Varietà per pianta"));
    ImGui::SameLine(S(210));
    ImGui::SetNextItemWidth(-FLT_MIN);
    f.changed |= ImGui::SliderFloat("##jit", &c.assetJitterDays, 0.0f, 7.0f, tr("+/- %.1f giorni"), ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip(tr("Sposta il calendario di ogni albero, così un bosco non cambia tutto insieme."));
}

void tabRegions(Frame& f) {
    SeasonConfig& c = f.cfg;
    dimText(tr("Ogni regione può essere spenta, anticipata o ritardata. Deserto e Abisso restano sempre vanilla di default."));
    ImGui::Dummy(ImVec2(0, S(4)));
    float cardW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    for (int r = 0; r < kRegions; ++r) {
        season::RegionConfig& rc = c.regions[r];
        ImGui::PushID(r);
        if (r % 2) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("card", ImVec2(cardW, S(150)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushFont(g_fontBold, 0.0f);
        ImGui::TextUnformatted(regionName(r));
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 1.6f);
        bool en = rc.enabled;
        if (toggle("##en", &en)) {
            rc.enabled = en;
            f.changed = true;
        }
        if (excludedByDefault(r)) {
            ImGui::PushStyleColor(ImGuiCol_Text, rc.enabled ? kWarn : kTextDim);
            ImGui::TextUnformatted(rc.enabled ? tr("esclusa di default: attivarla è sperimentale") : tr("esclusa di default (sempre vanilla)"));
            ImGui::PopStyleColor();
        } else {
            dimText("%s", regionHint(r));
        }
        ImGui::BeginDisabled(!rc.enabled);
        ImGui::SetNextItemWidth(-FLT_MIN);
        f.changed |= ImGui::SliderFloat("##off", &rc.dayOffset, -15.0f, 15.0f,
                                        rc.dayOffset < -0.05f ? tr("anticipo %.1f giorni") : (rc.dayOffset > 0.05f ? tr("ritardo %+.1f giorni") : tr("nessuno spostamento")),
                                        ImGuiSliderFlags_AlwaysClamp);
        float sw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5;
        for (int p = 0; p < season::kPhaseCount; ++p) {
            if (p) ImGui::SameLine();
            ImGui::PushID(p);
            ImGui::SetNextItemWidth(sw);
            // compact drag field with a colour chip for the phase
            ImVec2 fp = ImGui::GetCursorScreenPos();
            char fmt[16];
            snprintf(fmt, sizeof fmt, "x%%.1f");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(18), ImGui::GetStyle().FramePadding.y));
            f.changed |= ImGui::DragFloat("##sc", &rc.phaseScale[p], 0.01f, 0.0f, 3.0f, fmt, ImGuiSliderFlags_AlwaysClamp);
            ImGui::PopStyleVar();
            float fh = ImGui::GetFrameHeight();
            drawSeasonIcon(ImGui::GetWindowDrawList(), ImVec2(fp.x + S(11), fp.y + fh * 0.5f), fh * 0.28f, p);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip(tr("%s: durata x%.2f in questa regione (trascina)"), phaseName(p), rc.phaseScale[p]);
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void tabWeather(Frame& f) {
    SeasonConfig& c = f.cfg;
    float colW = ImGui::GetContentRegionAvail().x * 0.47f;
    ImGui::BeginGroup();
    ImGui::PushItemWidth(colW);
    sectionTitle(tr("Cosa controlla la mod"));
    f.changed |= toggle(tr("Meteo stagionale"), &c.weatherControl, tr("Pesi del meteo per stagione: estati limpide, autunni piovosi, inverni nevosi."));
    f.changed |= toggle(tr("Neve che si accumula"), &c.snowAccumulation, tr("La copertura cresce quando nevica sotto zero e si scioglie sopra zero."));
    f.changed |= toggle(tr("Luce stagionale"), &c.lightControl, tr("Sole più basso e toni freddi in inverno, più caldi in autunno."));
    f.changed |= toggle(tr("Durata del giorno"), &c.dayLengthControl, tr("Giornate più corte in inverno, più lunghe in estate."));
    f.changed |= toggle(tr("Particelle"), &c.particles, tr("Foglie che cadono in autunno, fiato visibile al freddo."));
    ImGui::Dummy(ImVec2(0, S(6)));
    sectionTitle(tr("Regolazioni"));
    ImGui::BeginDisabled(!c.snowAccumulation);
    f.changed |= ImGui::SliderFloat("##acc", &c.snowAccumPerHour, 0.0f, 1.0f, tr("accumulo %.2f / ora di gioco"), ImGuiSliderFlags_AlwaysClamp);
    f.changed |= ImGui::SliderFloat("##melt", &c.snowMeltPerHour, 0.0f, 1.0f, tr("scioglimento %.2f / ora"), ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!c.lightControl && !c.dayLengthControl);
    f.changed |= ImGui::SliderFloat("##lat", &c.virtualLatitudeDeg, 0.0f, 70.0f, tr("latitudine virtuale %.0f gradi"), ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    ImGui::SameLine(0, S(28));
    ImGui::BeginGroup();
    sectionTitle(tr("Dal vivo"));
    ImGui::SetNextItemWidth(S(200));
    if (ImGui::BeginCombo("##reg", regionName(g_liveRegion))) {
        for (int r = 0; r < kRegions; ++r)
            if (ImGui::Selectable(regionName(r), r == g_liveRegion)) g_liveRegion = r;
        ImGui::EndCombo();
    }
    const SeasonState& s = f.st;
    if (!f.engineLive) {
        dimText(tr("Motore stagioni in attesa: nessun valore ancora."));
    }
    char buf[48];
    if (f.engineLive) snprintf(buf, sizeof buf, "%.1f °C", s.temperatureC[g_liveRegion]);
    else snprintf(buf, sizeof buf, "-- °C");
    ImGui::PushFont(g_fontBold, ImGui::GetStyle().FontSizeBase * 1.6f);
    float t = s.temperatureC[g_liveRegion];
    ImVec4 tc = t < 0 ? ImVec4(0.62f, 0.80f, 0.96f, 1) : (t < 15 ? kText : kAccentHi);
    ImGui::TextColored(tc, "%s", buf);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    dimText(f.engineLive && t < s.breathThresholdC ? tr("fiato visibile") : tr("temperatura"));
    meter(tr("Manto nevoso"), s.snowCover[g_liveRegion], Rgb{0.85f, 0.92f, 1.0f});
    ImGui::Dummy(ImVec2(0, S(2)));
    const season::WeatherWeights& w = s.weather[g_liveRegion];
    meter(tr("Sereno"), w.sunny, Rgb{0.98f, 0.80f, 0.30f});
    meter(tr("Nuvoloso"), w.cloudy, Rgb{0.70f, 0.70f, 0.72f});
    meter(tr("Pioggia"), w.rainy, Rgb{0.40f, 0.62f, 0.90f});
    meter(tr("Temporale"), w.storm, Rgb{0.55f, 0.45f, 0.80f});
    meter(tr("Neve"), w.snowy, Rgb{0.88f, 0.94f, 1.00f});
    meter(tr("Vento"), w.windy, Rgb{0.55f, 0.80f, 0.70f});
    ImGui::Dummy(ImVec2(0, S(4)));
    char a[8], b[8];
    hourText(s.sunriseHour, a, sizeof a);
    hourText(s.sunsetHour, b, sizeof b);
    ImGui::Text(tr("Alba %s, tramonto %s"), a, b);
    dayBar(s.sunriseHour, s.sunsetHour);
    ImGui::EndGroup();
}

std::wstring logPath() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&logPath), &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    std::wstring s(p, n);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1)) + L"QuattroStagioni.log";
}

void tabInfo(Frame& f) {
    sectionTitle(tr("Versioni"));
    ImGui::Text("QuattroStagioni %s", g_modVersion.c_str());
    dimText(tr("Interfaccia: Dear ImGui %s (MIT), hook: MinHook (BSD)"), IMGUI_VERSION);
    ImGui::Dummy(ImVec2(0, S(6)));
    sectionTitle(tr("Stato"));
    ImGui::TextWrapped("%s", f.status.empty() ? tr("nessun messaggio") : f.status.c_str());
    ImGui::Text(tr("Overlay: %s"), overlayActive() ? tr("attivo") : tr("non agganciato"));
    ImGui::Text(tr("Orologio del gioco: %s"), f.clockHooked ? tr("agganciato") : tr("non agganciato"));
    ImGui::Text(tr("Motore stagioni: %s"), f.engineLive ? tr("attivo") : tr("in attesa"));
    ImGui::Dummy(ImVec2(0, S(6)));
    sectionTitle(tr("Pacchetto stagionale"));
    dimText("%s", tr("I pacchetti si creano sul tuo PC con qs_gen dai file del gioco: nessun asset viene distribuito."));
    ImGui::Dummy(ImVec2(0, S(6)));
    sectionTitle(tr("File di log"));
    std::wstring lp = logPath();
    char narrow[MAX_PATH * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, lp.c_str(), int(lp.size()), narrow, sizeof narrow - 1, nullptr, nullptr);
    narrow[n > 0 ? n : 0] = 0;
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextWrapped("%s", narrow);
    ImGui::PopStyleColor();
    if (ImGui::SmallButton(tr("Copia percorso"))) ImGui::SetClipboardText(narrow);
}

// ------------------------------------------------------------------ header and footer
void header(const Frame& f, bool* open) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth(), hh = S(92);
    int ph;
    float prog;
    phaseAt(f.cfg, displayDay(f), ph, prog);
    const Rgb& pc = kPhaseColor[ph];
    // header band with a soft tint of the season colour
    ImU32 l = col(scaled(pc, 0.34f), 0.95f), r = col(ImVec4(0.12f, 0.10f, 0.09f, 0.0f));
    dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + ww, wp.y + hh), l, r, r, l);
    dl->AddLine(ImVec2(wp.x, wp.y + hh), ImVec2(wp.x + ww, wp.y + hh), col(scaled(pc, 0.8f), 0.6f), S(1.5f));
    ImVec2 ic(wp.x + S(52), wp.y + hh * 0.5f);
    dl->AddCircleFilled(ic, S(33), col(ImVec4(0.08f, 0.07f, 0.06f, 0.85f)), 48);
    dl->AddCircle(ic, S(33), col(pc, 0.9f), 48, S(2));
    drawSeasonIcon(dl, ic, S(24), ph);
    ImGui::PushFont(g_fontBold, ImGui::GetStyle().FontSizeBase * 1.75f);
    ImVec2 tp(wp.x + S(100), wp.y + S(18));
    dl->AddText(tp, col(kText), "QuattroStagioni");
    float th = ImGui::GetFontSize();
    ImGui::PopFont();
    char sub[96];
    snprintf(sub, sizeof sub, tr("%s, giorno %d di %d"), phaseName(ph),
             int(std::floor(std::fmod(std::fmod(displayDay(f), yearLength(f.cfg)) + yearLength(f.cfg), yearLength(f.cfg)))) + 1,
             int(std::lround(yearLength(f.cfg))));
    dl->AddText(ImVec2(tp.x + S(2), tp.y + th + S(2)), col(pc), sub);
    // close button
    float cs = S(30);
    ImVec2 cp(wp.x + ww - cs - S(16), wp.y + S(16));
    ImGui::SetCursorScreenPos(cp);
    if (ImGui::InvisibleButton("##close", ImVec2(cs, cs))) *open = false;
    bool hov = ImGui::IsItemHovered();
    ImVec2 cc(cp.x + cs * 0.5f, cp.y + cs * 0.5f);
    if (hov) dl->AddCircleFilled(cc, cs * 0.5f, col(ImVec4(1, 1, 1, 0.10f)), 32);
    float k = cs * 0.22f;
    dl->AddLine(ImVec2(cc.x - k, cc.y - k), ImVec2(cc.x + k, cc.y + k), col(hov ? kText : kTextDim), S(2));
    dl->AddLine(ImVec2(cc.x - k, cc.y + k), ImVec2(cc.x + k, cc.y - k), col(hov ? kText : kTextDim), S(2));
    ImGui::SetCursorScreenPos(ImVec2(wp.x + ImGui::GetStyle().WindowPadding.x, wp.y + hh + S(10)));
}

void footer(Frame& f) {
    ImGuiStyle& st = ImGui::GetStyle();
    float fh = ImGui::GetFrameHeight() + st.WindowPadding.y;
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth(), wh = ImGui::GetWindowHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y0 = wp.y + wh - fh - st.WindowPadding.y * 0.5f;
    dl->AddLine(ImVec2(wp.x + st.WindowPadding.x, y0 - S(4)), ImVec2(wp.x + ww - st.WindowPadding.x, y0 - S(4)), col(kPanelHi), S(1));
    ImGui::SetCursorScreenPos(ImVec2(wp.x + st.WindowPadding.x, y0 + S(2)));
    double now = ImGui::GetTime();
    ImGui::AlignTextToFramePadding();
    if (now - g_saveTime < 4.0) {
        ImGui::TextColored(g_saveOk ? ImVec4(0.60f, 0.85f, 0.45f, 1) : kWarn, "%s", g_saveMsg.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::Text("%s", f.status.empty() ? tr("F5 o Esc per chiudere. Le modifiche si applicano subito.") : f.status.c_str());
        ImGui::PopStyleColor();
    }
    float bw = S(130);
    ImGui::SameLine(ww - bw - st.WindowPadding.x);
    bool canSave = g_saveFn != nullptr;
    ImGui::BeginDisabled(!canSave);
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHi);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.48f, 0.18f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.08f, 0.05f, 1));
    if (ImGui::Button(tr("Salva"), ImVec2(bw, 0))) {
        std::string err;
        g_saveOk = menuRequestSave(err);
        g_saveMsg = g_saveOk ? tr("Impostazioni salvate in QuattroStagioni.ini") : (tr("Salvataggio non riuscito: ") + err);
        g_saveTime = now;
    }
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(tr("Salvataggio non ancora collegato in questa versione"));
}

}  // namespace

// ------------------------------------------------------------------ public (overlay.h)
void menuSetSaveCallback(MenuSaveFn fn) { g_saveFn = fn; }

bool menuRequestSave(std::string& error) {
    if (!g_saveFn) {
        error = tr("nessuna funzione di salvataggio");
        return false;
    }
    return g_saveFn(error);
}

void menuSetVersionInfo(const char* v) { g_modVersion = v ? v : "?"; }

namespace menu {

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Start with a large atlas: every growth re-creates the GPU texture (a synchronous upload on the
    // game's queue), so we want it to happen rarely, ideally never after the first frames.
    io.Fonts->TexMinWidth = 1024;
    io.Fonts->TexMinHeight = 1024;
    wchar_t win[MAX_PATH];
    UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    std::string fonts;
    if (n && n < MAX_PATH) {
        char b[MAX_PATH * 3];
        int k = WideCharToMultiByte(CP_UTF8, 0, win, int(n), b, sizeof b - 1, nullptr, nullptr);
        b[k > 0 ? k : 0] = 0;
        fonts = std::string(b) + "\\Fonts\\";
    }
    auto tryLoad = [&](std::initializer_list<const char*> names) -> ImFont* {
        for (const char* nm : names) {
            std::string p = fonts + nm;
            std::wstring wp(p.begin(), p.end());
            if (GetFileAttributesW(wp.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig fc;
            fc.OversampleH = 2;
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(p.c_str(), 18.0f, &fc)) return f;
        }
        return nullptr;
    };
    g_fontBody = tryLoad({"segoeui.ttf", "selawk.ttf", "calibri.ttf", "Carlito-Regular.ttf", "tahoma.ttf", "arial.ttf"});
    g_fontBold = tryLoad({"seguisb.ttf", "segoeuib.ttf", "calibrib.ttf", "Carlito-Bold.ttf", "tahomabd.ttf", "arialbd.ttf"});
    if (!g_fontBody) g_fontBody = io.Fonts->AddFontDefault();
    if (!g_fontBold) g_fontBold = g_fontBody;
    io.FontDefault = g_fontBody;
}

void applyStyle(float scale) {
    g_scale = scale;
    g_prewarmed = false;
    ImGuiStyle st;  // fresh defaults, then our look, then scale
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 14;
    st.ChildRounding = 10;
    st.FrameRounding = 7;
    st.PopupRounding = 8;
    st.GrabRounding = 7;
    st.TabRounding = 8;
    st.ScrollbarRounding = 8;
    st.WindowBorderSize = 1;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.TabBarBorderSize = 1;
    st.TabBarOverlineSize = 2;
    st.WindowPadding = ImVec2(20, 16);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.GrabMinSize = 12;
    st.ScrollbarSize = 12;
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.44f, 0.39f, 1);
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.11f, 0.095f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.36f, 0.29f, 0.22f, 0.65f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.17f, 0.145f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.22f, 0.18f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.31f, 0.25f, 0.20f, 1);
    c[ImGuiCol_TitleBg] = kPanel;
    c[ImGuiCol_TitleBgActive] = kPanelHi;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.32f, 0.27f, 0.22f, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.42f, 0.35f, 0.28f, 1);
    c[ImGuiCol_ScrollbarGrabActive] = kAccent;
    c[ImGuiCol_CheckMark] = kAccentHi;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHi;
    c[ImGuiCol_Button] = ImVec4(0.24f, 0.20f, 0.17f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.27f, 0.21f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.42f, 0.33f, 0.24f, 1);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.18f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.38f, 0.30f, 0.22f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.46f, 0.35f, 0.24f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.32f, 0.27f, 0.22f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.17f, 0.145f, 0.125f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.36f, 0.27f, 0.19f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.22f, 0.15f, 1);
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
    c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_TabDimmedSelectedOverline] = kAccent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.90f, 0.58f, 0.24f, 0.35f);
    c[ImGuiCol_NavCursor] = kAccentHi;
    st.ScaleAllSizes(scale);
    st.FontSizeBase = 18.0f;
    st.FontScaleMain = scale;
    ImGui::GetStyle() = st;
}

void draw(bool* open, float fade) {
    prewarmFonts();
    Frame f;
    {
        SharedState& s = shared();
        std::lock_guard<std::mutex> lk(s.mutex);
        f.cfg = s.config;
        f.st = s.season;
        f.status = s.statusLine;
    }
    f.clockHooked = shared().gameClockHooked.load();
    float wsum = 0;
    for (const auto& w : f.st.weather) wsum += w.sunny + w.cloudy + w.rainy + w.storm + w.snowy + w.windy;
    f.engineLive = f.st.day != 0 || wsum > 0;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 size(std::min(S(900), io.DisplaySize.x - S(40)), std::min(S(660), io.DisplaySize.y - S(40)));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    // soft shadow behind the window
    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        for (int i = 0; i < 8; ++i) {
            float g = S(2 + i * 3);
            bg->AddRectFilled(ImVec2(c.x - size.x * 0.5f - g, c.y - size.y * 0.5f - g + S(10)),
                              ImVec2(c.x + size.x * 0.5f + g, c.y + size.y * 0.5f + g + S(10)),
                              col(ImVec4(0, 0, 0, 0.045f * fade * (1.0f - i / 8.0f))), S(14) + g);
        }
    }
    if (ImGui::Begin("QuattroStagioni##menu", nullptr, wf)) {
        header(f, open);
        ImGuiStyle& st = ImGui::GetStyle();
        float footerH = ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f + S(6);
        if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
            struct Tab {
                const char* name;
                void (*fn)(Frame&);
            } tabs[] = {{tr("  Calendario  "), tabCalendar}, {tr("  Stagioni  "), tabSeasons}, {tr("  Regioni  "), tabRegions},
                        {tr("  Meteo e luce  "), tabWeather}, {tr("  Info  "), tabInfo}};
            for (const Tab& t : tabs) {
                if (ImGui::BeginTabItem(t.name)) {
                    ImGui::Dummy(ImVec2(0, S(4)));
                    float h = ImGui::GetContentRegionAvail().y - footerH;
                    ImGui::BeginChild("content", ImVec2(0, h), ImGuiChildFlags_None);
                    t.fn(f);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        footer(f);
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (f.changed) {
        SharedState& s = shared();
        std::lock_guard<std::mutex> lk(s.mutex);
        s.config = f.cfg;
        s.configDirty.store(true);
    }
}

void drawToast(float alpha, const char* keyName) {
    prewarmFonts();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    char line[96];
    snprintf(line, sizeof line, tr("Premi %s per il menu"), keyName);
    ImGui::PushFont(g_fontBold, 0.0f);
    ImVec2 t1 = ImGui::CalcTextSize("QuattroStagioni attiva");
    ImGui::PopFont();
    ImVec2 t2 = ImGui::CalcTextSize(line);
    float w = std::max(t1.x, t2.x) + S(84), h = S(64);
    float slide = (1.0f - alpha) * S(16);
    ImVec2 p(io.DisplaySize.x - w - S(28) + slide, S(28));
    dl->AddRectFilled(ImVec2(p.x + S(3), p.y + S(5)), ImVec2(p.x + w + S(3), p.y + h + S(5)), col(ImVec4(0, 0, 0, 0.35f * alpha)), S(12));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(kBg, alpha), S(12));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), col(kAccent, 0.55f * alpha), S(12), 0, S(1.2f));
    int ph;
    float prog;
    SeasonConfig cfg;
    float day;
    {
        std::lock_guard<std::mutex> lk(shared().mutex);
        cfg = shared().config;
        day = cfg.daySource == DaySource::Manual ? cfg.startDay : shared().season.dayOfYear;
    }
    phaseAt(cfg, day, ph, prog);
    drawSeasonIcon(dl, ImVec2(p.x + S(34), p.y + h * 0.5f), S(17), ph, alpha);
    ImGui::PushFont(g_fontBold, 0.0f);
    dl->AddText(ImVec2(p.x + S(64), p.y + S(11)), col(kText, alpha), "QuattroStagioni attiva");
    ImGui::PopFont();
    dl->AddText(ImVec2(p.x + S(64), p.y + S(13) + t1.y), col(kTextDim, alpha), line);
}

}  // namespace menu
}  // namespace qs
