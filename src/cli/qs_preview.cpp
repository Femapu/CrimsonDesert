// QuattroStagioni - qs_preview: contact sheets of the seasonal recipes, and fit tests.
//
// Stefano runs this on the PC on real extracted textures to judge the recipes by eye (the repo only
// ships synthetic images). It reads a PNG or a DDS (plain, or the game's stored per-mip LZ4 form),
// applies every look and transition stage of a profile for a few seeds and writes one PNG sheet.
// With --fit it also re-encodes each look and checks that every LZ4 mip still fits the original
// stored size (what the generator must do), printing the fitBias needed.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../core/dds.h"
#include "../core/lz4_exact.h"
#include "../core/recipe/recipe.h"
#include "../core/texture/synth.h"
#include "../core/texture/texture.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "stb_easy_font.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using qs::recipe::Look;
using qs::recipe::Stage;
using qs::tex::Image;

namespace {

const char* kUsage =
    "qs_preview - anteprima delle ricette stagionali QuattroStagioni\n"
    "\n"
    "Uso:\n"
    "  qs_preview [opzioni] <immagine.png | texture.dds>\n"
    "  qs_preview [opzioni] --synth <leaves|grass|flowers|terrain|bark|conifer|climate>\n"
    "\n"
    "Opzioni:\n"
    "  --profile P     profilo della ricetta (deciduous, conifer, grass, flower, ...). Default: dal tipo\n"
    "                  sintetico, altrimenti deciduous\n"
    "  --seed N[,N..]  semi (varieta per asset, es. palette autunnale). Default: 1,2,3\n"
    "  --asset NOME    seme calcolato dal nome dell'asset come fa il generatore (lookup3)\n"
    "  --stages N      stadi intermedi per ogni transizione (default 2, 0 = solo le 5 look)\n"
    "  --thumb PX      lato delle miniature (default 200)\n"
    "  --recipes DIR   cartella delle ricette .ini (default data/recipes)\n"
    "  --out FILE      PNG di uscita (default preview_<profilo>.png)\n"
    "  --encode        mostra il risultato dopo la compressione BC (come in gioco)\n"
    "  --format F      formato per --encode / --fit su PNG: bc1 o bc3 (default: bc1 se l'alfa e' 0/255)\n"
    "  --bg B          sfondo sotto l'alfa: sky (default), checker, grey, black\n"
    "  --size PX       lato delle immagini sintetiche (default 256)\n"
    "  --fit           ricodifica ogni look e verifica che ogni mip LZ4 rientri nella dimensione\n"
    "                  originale (su PNG/sintetici: originale simulato con LZ4 standard)\n"
    "  --fit-hc        come --fit ma con originale simulato compresso LZ4-HC (caso peggiore)\n"
    "  --list          elenca i profili disponibili\n"
    "\n"
    "Esempi:\n"
    "  qs_preview --synth leaves --stages 3\n"
    "  qs_preview foglia_quercia.dds --profile deciduous --seed 1,2,3,4 --encode --fit\n";

std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool endsWith(std::string s, const std::string& suf) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// ---- drawing ----------------------------------------------------------------------------------------
struct Sheet {
    uint32_t w, h;
    std::vector<uint8_t> rgb;
    Sheet(uint32_t W, uint32_t H, uint8_t bg) : w(W), h(H), rgb(size_t(W) * H * 3, bg) {}
    void px(int x, int y, const uint8_t c[3]) {
        if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
        std::memcpy(&rgb[(size_t(y) * w + x) * 3], c, 3);
    }
    void rect(int x0, int y0, int x1, int y1, const uint8_t c[3]) {
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) px(x, y, c);
    }
    // stb_easy_font text, scaled.
    void text(int x, int y, const std::string& s, const uint8_t c[3], int scale = 2) {
        static char buf[60000];
        unsigned char col[4] = {255, 255, 255, 255};
        std::string t = s;
        int quads = stb_easy_font_print(0, 0, &t[0], col, buf, sizeof(buf));
        for (int q = 0; q < quads; ++q) {
            const float* v = reinterpret_cast<const float*>(buf + q * 64);
            float qx0 = v[0], qy0 = v[1], qx1 = v[8], qy1 = v[9];
            rect(x + int(qx0 * scale), y + int(qy0 * scale), x + int(qx1 * scale), y + int(qy1 * scale), c);
        }
    }
};

// Area resample to fit in `size` x `size` (alpha weighted), keeps aspect.
Image thumbnail(const Image& src, uint32_t size) {
    double sc = double(size) / std::max(src.width, src.height);
    Image d;
    d.width = std::max(1u, uint32_t(src.width * sc + 0.5));
    d.height = std::max(1u, uint32_t(src.height * sc + 0.5));
    d.rgba.assign(size_t(d.width) * d.height * 4, 0);
    for (uint32_t y = 0; y < d.height; ++y)
        for (uint32_t x = 0; x < d.width; ++x) {
            uint32_t sx0 = uint32_t(x / sc), sx1 = std::max(sx0 + 1, std::min(src.width, uint32_t((x + 1) / sc)));
            uint32_t sy0 = uint32_t(y / sc), sy1 = std::max(sy0 + 1, std::min(src.height, uint32_t((y + 1) / sc)));
            double acc[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t yy = sy0; yy < sy1 && yy < src.height; ++yy)
                for (uint32_t xx = sx0; xx < sx1 && xx < src.width; ++xx) {
                    const uint8_t* p = &src.rgba[(size_t(yy) * src.width + xx) * 4];
                    double a = p[3] / 255.0;
                    for (int k = 0; k < 3; ++k) acc[k] += p[k] * a;
                    acc[3] += a;
                    n += 1;
                }
            uint8_t* o = &d.rgba[(size_t(y) * d.width + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = uint8_t(acc[3] > 0 ? acc[k] / acc[3] + 0.5 : 0);
            o[3] = uint8_t(n > 0 ? acc[3] / n * 255 + 0.5 : 0);
        }
    return d;
}

void blit(Sheet& s, const Image& img, int ox, int oy, const std::string& bg) {
    for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x) {
            const uint8_t* p = &img.rgba[(size_t(y) * img.width + x) * 4];
            double b[3];
            if (bg == "checker") { double v = ((x / 8 + y / 8) & 1) ? 200 : 150; b[0] = b[1] = b[2] = v; }
            else if (bg == "grey") { b[0] = b[1] = b[2] = 128; }
            else if (bg == "black") { b[0] = b[1] = b[2] = 0; }
            else { double t = double(y) / img.height; b[0] = 150 + 40 * t; b[1] = 186 + 26 * t; b[2] = 222 + 14 * t; }
            double a = p[3] / 255.0;
            uint8_t c[3];
            for (int k = 0; k < 3; ++k) c[k] = uint8_t(p[k] * a + b[k] * (1 - a) + 0.5);
            s.px(ox + int(x), oy + int(y), c);
        }
}

// ---- input ------------------------------------------------------------------------------------------
struct Input {
    qs::tex::Texture tex;      // decoded texture (DDS) or a single-mip RGBA8 wrapper (PNG/synth)
    bool fromDds = false;
    std::vector<uint8_t> plain;  // plain DDS bytes (DDS input)
    qs::DdsInfo info;
    std::string name;
};

bool loadInput(const std::string& path, Input& in, std::string& err) {
    in.name = path;
    std::vector<uint8_t> data = readFile(path);
    if (data.empty()) { err = "cannot read " + path; return false; }
    if (endsWith(path, ".dds")) {
        if (!qs::parseDdsHeader(data.data(), data.size(), in.info, &err)) return false;
        size_t plainSize = in.info.headerSize;
        for (uint32_t s : in.info.mipRawSize) plainSize += s;
        if (data.size() < plainSize) {  // stored per-mip LZ4 form (as in the .paz)
            std::vector<uint8_t> dec;
            if (!qs::validatePerMipDds(data.data(), data.size(), plainSize, &err, &dec)) {
                err = "DDS is neither plain nor a valid per-mip stored blob: " + err;
                return false;
            }
            data.swap(dec);
        }
        if (!qs::tex::decodeDds(data.data(), data.size(), in.tex, &err)) return false;
        in.plain.swap(data);
        in.fromDds = true;
        return true;
    }
    int w, h, n;
    unsigned char* p = stbi_load_from_memory(data.data(), int(data.size()), &w, &h, &n, 4);
    if (!p) { err = std::string("cannot decode image: ") + stbi_failure_reason(); return false; }
    Image img;
    img.width = uint32_t(w);
    img.height = uint32_t(h);
    img.rgba.assign(p, p + size_t(w) * h * 4);
    stbi_image_free(p);
    in.tex.format = qs::tex::Format::RGBA8;
    in.tex.mips.push_back(std::move(img));
    return true;
}

bool binaryAlpha(const Image& img) {
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] != 0 && img.rgba[i] != 255) return false;
    return true;
}

// Texture with a full mip chain in a BC format (simulates a game texture for PNG/synthetic input).
qs::tex::Texture asBlockTexture(const Image& img, qs::tex::Format f) {
    qs::tex::Texture t;
    t.format = f;
    t.mips.push_back(img);
    uint32_t w = img.width, h = img.height;
    while (w > 1 || h > 1) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        t.mips.push_back(Image());
    }
    qs::tex::rebuildMips(t, true);
    return t;
}

Image roundTrip(const Image& img, qs::tex::Format f) {
    std::vector<uint8_t> blocks;
    qs::tex::EncodeOptions o;
    qs::tex::encodeSurface(f, img, o, blocks);
    Image d;
    qs::tex::decodeSurface(f, blocks.data(), blocks.size(), img.width, img.height, d);
    return d;
}

std::string stageLabel(const Stage& s) {
    char b[32];
    if (s.from == s.to || s.t == 0) { std::snprintf(b, sizeof b, "%c", qs::recipe::toChar(s.from)); return b; }
    std::snprintf(b, sizeof b, "%c>%c %.2f", qs::recipe::toChar(s.from), qs::recipe::toChar(s.to), s.t);
    return b;
}

// ---- fit test ---------------------------------------------------------------------------------------
void fitTest(const Input& in, const qs::recipe::RecipeBook& book, const qs::recipe::Context& ctx0,
             qs::tex::Format simFormat, bool simHc) {
    using namespace qs::tex;
    Texture orig;
    std::vector<std::vector<uint8_t>> origMips;
    std::vector<size_t> targets;
    if (in.fromDds) {
        orig = in.tex;
        size_t p = in.info.headerSize;
        for (size_t i = 0; i < orig.mips.size(); ++i) {
            size_t raw = in.info.mipRawSize[i];
            origMips.emplace_back(in.plain.begin() + long(p), in.plain.begin() + long(p + raw));
            size_t st = i < 4 ? in.info.storedMip[i] : raw;
            targets.push_back(st == 0 ? raw : st);
            p += raw;
        }
        std::printf("fit test on %s (%s %ux%u, %zu mips); targets = stored sizes from the DDS header\n", in.name.c_str(),
                    toString(orig.format), in.info.width, in.info.height, orig.mips.size());
    } else {
        orig = asBlockTexture(in.tex.mips[0], simFormat);
        for (size_t i = 0; i < orig.mips.size(); ++i) {
            std::vector<uint8_t> b;
            EncodeOptions o;
            o.level = 0;  // a plain encoder stands in for the original tool
            encodeMip(orig, i, o, b);
            size_t def = simHc ? lz4BestSize(b.data(), b.size()) : qs::lz4DefaultSize(b.data(), b.size());
            targets.push_back(i < 4 && def < b.size() ? def : b.size());
            Image d;
            decodeSurface(orig.format, b.data(), b.size(), orig.mips[i].width, orig.mips[i].height, d);
            orig.mips[i] = d;
            origMips.push_back(std::move(b));
        }
        std::printf("fit test on %s (simulated %s original, targets = LZ4 %s size of a level-0 encode)\n",
                    in.name.c_str(), toString(orig.format), simHc ? "HC" : "default");
    }
    if (blockBytes(orig.format) == 0) std::printf("  note: raw format, fitBias has no effect\n");
    const Look looks[4] = {Look::Spring, Look::LateSummer, Look::Autumn, Look::Winter};
    for (Look l : looks) {
        Stage st = qs::recipe::pureStage(l);
        if (!book.affects(ctx0, st)) { std::printf("  %c: unchanged (vanilla bytes)\n", qs::recipe::toChar(l)); continue; }
        Texture t = orig;
        std::string err;
        if (!book.apply(ctx0, st, t.mips[0], &err)) { std::printf("  %c: recipe error: %s\n", qs::recipe::toChar(l), err.c_str()); continue; }
        rebuildMips(t, true);
        qs::recipe::PaletteBinding pb;
        pb.book = &book;
        pb.ctx = ctx0;
        pb.stage = st;
        RecolorFallback rf{&qs::recipe::paletteThunk, &pb};
        std::printf("  %c:", qs::recipe::toChar(l));
        for (size_t i = 0; i < t.mips.size() && i < 4; ++i) {
            if (targets[i] >= origMips[i].size()) { std::printf("  mip%zu raw", i); continue; }
            auto t0 = std::chrono::steady_clock::now();
            FitResult r = fitMip(t, i, targets[i], origMips[i].data(), origMips[i].size(), EncodeOptions(), &rf);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (r.ok) std::printf("  mip%zu ok fb=%d (%zu/%zu, %.0f ms)", i, r.fitBias, r.hcSize, targets[i], ms);
            else std::printf("  mip%zu FAIL (%s)", i, r.error.c_str());
        }
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string input, synthKind, profile, recipes = "data/recipes", out, format, bg = "sky", asset;
    std::vector<uint32_t> seeds;
    int stages = 2, thumb = 200, size = 256;
    bool encode = false, fit = false, fitHc = false, list = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { std::fputs(kUsage, stdout); return 0; }
        else if (a == "--synth") synthKind = next("--synth");
        else if (a == "--profile") profile = next("--profile");
        else if (a == "--seed") {
            std::string s = next("--seed");
            size_t p = 0;
            while (p < s.size()) {
                size_t e = s.find(',', p);
                if (e == std::string::npos) e = s.size();
                seeds.push_back(uint32_t(std::strtoul(s.substr(p, e - p).c_str(), nullptr, 0)));
                p = e + 1;
            }
        }
        else if (a == "--asset") asset = next("--asset");
        else if (a == "--stages") stages = std::atoi(next("--stages").c_str());
        else if (a == "--thumb") thumb = std::atoi(next("--thumb").c_str());
        else if (a == "--size") size = std::atoi(next("--size").c_str());
        else if (a == "--recipes") recipes = next("--recipes");
        else if (a == "--out") out = next("--out");
        else if (a == "--format") format = next("--format");
        else if (a == "--bg") bg = next("--bg");
        else if (a == "--encode") encode = true;
        else if (a == "--fit") fit = true;
        else if (a == "--fit-hc") fit = fitHc = true;
        else if (a == "--list") list = true;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n\n%s", a.c_str(), kUsage); return 2; }
        else input = a;
    }
    qs::recipe::RecipeBook book;
    std::vector<std::string> errs;
    if (!book.load(recipes, errs)) {
        std::fprintf(stderr, "recipe errors:\n");
        for (auto& e : errs) std::fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }
    if (list) {
        for (auto& p : book.profiles()) {
            std::printf("%-20s", p.c_str());
            for (Look l : {Look::Spring, Look::LateSummer, Look::Autumn, Look::Winter})
                std::printf(" %c", book.changesLook(p, l) ? qs::recipe::toChar(l) : '.');
            std::printf("\n");
        }
        return 0;
    }
    if (input.empty() && synthKind.empty()) { std::fputs(kUsage, stdout); return 2; }
    if (stages < 0 || stages > 8 || thumb < 32 || thumb > 1024 || size < 16 || size > 4096) {
        std::fprintf(stderr, "value out of range (--stages 0..8, --thumb 32..1024, --size 16..4096)\n");
        return 2;
    }

    Input in;
    std::string err;
    if (!synthKind.empty()) {
        qs::tex::synth::Kind k;
        if (!qs::tex::synth::kindFromString(synthKind, k)) { std::fprintf(stderr, "unknown synthetic kind %s\n", synthKind.c_str()); return 2; }
        in.tex.format = qs::tex::Format::RGBA8;
        in.tex.mips.push_back(qs::tex::synth::make(k, uint32_t(size), 1));
        in.name = "synthetic " + synthKind;
        if (profile.empty()) profile = qs::tex::synth::defaultProfile(k);
    } else if (!loadInput(input, in, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (profile.empty()) profile = "deciduous";
    if (!book.hasProfile(profile)) { std::fprintf(stderr, "unknown profile %s (use --list)\n", profile.c_str()); return 2; }
    if (!asset.empty()) seeds.insert(seeds.begin(), qs::recipe::seedForAsset(asset));
    if (seeds.empty()) seeds = {1, 2, 3};
    if (out.empty()) out = "preview_" + profile + ".png";

    const Image& base = in.tex.mips[0];
    qs::tex::Format encFormat = in.tex.format;
    if (!in.fromDds || qs::tex::blockBytes(encFormat) == 0) {
        if (format == "bc3") encFormat = qs::tex::Format::BC3;
        else if (format == "bc1") encFormat = qs::tex::Format::BC1;
        else encFormat = binaryAlpha(base) ? qs::tex::Format::BC1 : qs::tex::Format::BC3;
    }

    // Columns: the yearly cycle S -> U -> L -> A -> W -> S with `stages` intermediate steps each.
    std::vector<Stage> cols;
    const Look cyc[5] = {Look::Spring, Look::Summer, Look::LateSummer, Look::Autumn, Look::Winter};
    for (int i = 0; i < 5; ++i) {
        cols.push_back(qs::recipe::pureStage(cyc[i]));
        for (int k = 1; k <= stages; ++k) cols.push_back(Stage{cyc[i], cyc[(i + 1) % 5], float(k) / float(stages + 1)});
    }
    const int pad = 8, labelH = 30, headH = 40, rowLabelW = 90;
    Image t0 = thumbnail(base, uint32_t(thumb));
    const int cw = int(t0.width), ch = int(t0.height);
    Sheet sheet(uint32_t(rowLabelW + int(cols.size()) * (cw + pad) + pad),
                uint32_t(headH + labelH + int(seeds.size()) * (ch + pad) + pad), 34);
    const uint8_t white[3] = {235, 235, 235}, grey[3] = {150, 150, 150}, accent[3] = {240, 190, 90};
    char hdr[256];
    std::snprintf(hdr, sizeof hdr, "QuattroStagioni  profile: %s   %s%s", profile.c_str(), in.name.c_str(),
                  encode ? "   (BC round trip)" : "");
    sheet.text(pad, 10, hdr, white);
    for (size_t c = 0; c < cols.size(); ++c) {
        bool pure = cols[c].t == 0;
        sheet.text(rowLabelW + int(c) * (cw + pad) + 4, headH + 4, stageLabel(cols[c]), pure ? accent : grey);
    }
    auto t00 = std::chrono::steady_clock::now();
    for (size_t r = 0; r < seeds.size(); ++r) {
        qs::recipe::Context ctx;
        ctx.profile = profile;
        ctx.seed = seeds[r];
        ctx.path = in.name;
        char sl[32];
        std::snprintf(sl, sizeof sl, "seed %u", seeds[r]);
        int y = headH + labelH + int(r) * (ch + pad);
        sheet.text(pad, y + ch / 2 - 8, sl, white);
        for (size_t c = 0; c < cols.size(); ++c) {
            Image img = base;
            if (!book.apply(ctx, cols[c], img, &err)) { std::fprintf(stderr, "recipe error: %s\n", err.c_str()); return 1; }
            if (encode) img = roundTrip(img, encFormat);
            blit(sheet, thumbnail(img, uint32_t(thumb)), rowLabelW + int(c) * (cw + pad), y, bg);
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t00).count();
    if (!stbi_write_png(out.c_str(), int(sheet.w), int(sheet.h), 3, sheet.rgb.data(), int(sheet.w) * 3)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%zu stages x %zu seeds, %.0f ms)\n", out.c_str(), cols.size(), seeds.size(), ms);
    if (fit) {
        qs::recipe::Context ctx;
        ctx.profile = profile;
        ctx.seed = seeds[0];
        fitTest(in, book, ctx, encFormat, fitHc);
    }
    return 0;
}
