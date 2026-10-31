// QuattroStagioni - unit tests of the DLC area polygon (src/core/apply/dlc_area.h).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/core/apply/dlc_area.h"

using namespace qs;
using namespace qs::apply;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (cond) ++g_pass;                                                                 \
        else {                                                                              \
            ++g_fail;                                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
        }                                                                                   \
    } while (0)

static bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

int main(int, char**) {
    // ---- parsePoints
    std::vector<WorldPoint> p;
    std::string why;
    CHECK(parsePoints("", p) && p.empty());
    CHECK(parsePoints("  ", p) && p.empty());
    CHECK(parsePoints("0,0; 100,0; 100,100; 0,100", p) && p.size() == 4);
    CHECK(p.size() == 4 && near(p[1].x, 100) && near(p[1].z, 0) && near(p[3].z, 100));
    CHECK(parsePoints("-1500.5, 2300.25;3000 -40;", p) && p.size() == 2);
    CHECK(p.size() == 2 && near(p[0].x, -1500.5) && near(p[0].z, 2300.25) && near(p[1].z, -40));
    CHECK(!parsePoints("1,2; 3", p, &why) && p.empty() && why.find("point 2") != std::string::npos);
    CHECK(!parsePoints("1,2,3", p));
    CHECK(!parsePoints("a,b", p));
    CHECK(!parsePoints("1,5,2", p));  // decimal comma is not accepted (ambiguous)
    CHECK(parsePoints("10,20; 30,40", p) && formatPoints(p) == "10, 20; 30, 40");

    // ---- insidePolygon (square 0..100 and a concave "L")
    std::vector<WorldPoint> sq;
    parsePoints("0,0; 100,0; 100,100; 0,100", sq);
    CHECK(insidePolygon(sq, 50, 50));
    CHECK(insidePolygon(sq, 1, 99));
    CHECK(!insidePolygon(sq, -1, 50));
    CHECK(!insidePolygon(sq, 50, 101));
    CHECK(!insidePolygon(sq, NAN, 50));
    std::vector<WorldPoint> two(sq.begin(), sq.begin() + 2);
    CHECK(!insidePolygon(two, 50, 0));
    std::vector<WorldPoint> L;
    parsePoints("0,0; 200,0; 200,100; 100,100; 100,200; 0,200", L);
    CHECK(insidePolygon(L, 50, 150));
    CHECK(insidePolygon(L, 150, 50));
    CHECK(!insidePolygon(L, 150, 150));  // the notch

    // ---- distanceToPolygon
    CHECK(near(distanceToPolygon(sq, 50, 50), 50));
    CHECK(near(distanceToPolygon(sq, -30, 50), 30));
    CHECK(near(distanceToPolygon(sq, 103, 104), 5));  // nearest is the corner (100, 100)
    CHECK(std::isinf(distanceToPolygon({}, 0, 0)));

    // ---- dlcInside: inactive, entering, margin on the way out
    DlcAreaConfig c;
    CHECK(!c.active());
    CHECK(!dlcInside(c, 50, 50, false));  // empty Points: nothing excluded
    c.points = sq;
    c.marginM = 20;
    CHECK(c.active());
    CHECK(dlcInside(c, 50, 50, false));
    CHECK(!dlcInside(c, 110, 50, false));  // outside, not yet inside: base game
    CHECK(dlcInside(c, 110, 50, true));    // inside the margin on the way out: still DLC
    CHECK(!dlcInside(c, 130, 50, true));   // past the margin: left
    CHECK(!dlcInside(c, NAN, 50, true));
    c.enabled = false;
    CHECK(!dlcInside(c, 50, 50, true));

    // ---- load
    Ini ini;
    ini.parse("[DLCArea]\nEnabled=1\nPoints=0,0; 100,0; 100,100\nMargin=75\nLogIntervalSec=5\n");
    DlcAreaConfig d;
    std::vector<std::string> warn;
    d.load(ini, warn);
    CHECK(warn.empty());
    CHECK(d.enabled && d.points.size() == 3 && near(d.marginM, 75) && near(d.logIntervalSec, 5));
    CHECK(d.active() && dlcInside(d, 90, 10, false));
    Ini empty;
    empty.parse("[DLCArea]\nEnabled=1\nPoints=\n");
    DlcAreaConfig e;
    warn.clear();
    e.load(empty, warn);
    CHECK(warn.empty() && !e.active());
    Ini bad;
    bad.parse("[DLCArea]\nPoints=1,2; 3,4\nMargin=x\n");
    DlcAreaConfig b;
    warn.clear();
    b.load(bad, warn);
    CHECK(warn.size() == 2 && !b.active() && near(b.marginM, 50));

    // ---- the shipped QuattroStagioni.ini: [DLCArea] present, clean, inactive until Points is filled
    FILE* f = std::fopen("dist/bin64/scripts/QuattroStagioni.ini", "rb");
    if (!f) {
        std::printf("  (skipping shipped QuattroStagioni.ini check: not found from cwd)\n");
    } else {
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        Ini shipped;
        shipped.parse(text);
        CHECK(shipped.has("DLCArea", "Enabled") && shipped.has("DLCArea", "Points"));
        DlcAreaConfig s;
        warn.clear();
        s.load(shipped, warn);
        CHECK(warn.empty() && s.enabled && !s.active());
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
