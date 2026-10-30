// QuattroStagioni - stage ids. See stage.h.
#include "stage.h"

#include "inventory.h"

namespace qs::gen {
namespace {
bool isLook(char c) { return c == 'S' || c == 'U' || c == 'L' || c == 'A' || c == 'W'; }
char upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; }
}  // namespace

char nextLook(char l) {
    switch (l) {
        case 'S': return 'U';
        case 'U': return 'L';
        case 'L': return 'A';
        case 'A': return 'W';
        default: return 'S';
    }
}

std::string StageSpec::id() const {
    if (plateau()) return std::string(1, from);
    return std::string(1, from) + ">" + std::string(1, to) + "." + std::to_string(step);
}

std::string StageSpec::fileName() const {
    if (plateau()) return std::string(1, from) + ".qsp";
    return std::string(1, from) + "-" + std::string(1, to) + "." + std::to_string(step) + ".qsp";
}

bool parseStage(const std::string& idIn, int steps, StageSpec& out, std::string* error) {
    std::string id = idIn;
    if (id.size() > 4 && (id.compare(id.size() - 4, 4, ".qsp") == 0 || id.compare(id.size() - 4, 4, ".QSP") == 0))
        id.resize(id.size() - 4);
    auto bad = [&](const std::string& m) {
        if (error) *error = "stage \"" + idIn + "\": " + m;
        return false;
    };
    if (steps < 1 || steps > 16) return bad("transition steps must be 1..16");
    StageSpec s;
    s.steps = steps;
    if (id.size() == 1) {
        char c = upper(id[0]);
        if (!isLook(c)) return bad("unknown look (use S U L A W)");
        s.from = s.to = c;
        out = s;
        return true;
    }
    if (id.size() < 5 || (id[1] != '>' && id[1] != '-') || id[3] != '.') return bad("expected X or X>Y.N");
    s.from = upper(id[0]);
    s.to = upper(id[2]);
    if (!isLook(s.from) || !isLook(s.to)) return bad("unknown look (use S U L A W)");
    if (nextLook(s.from) != s.to) return bad("transitions go S>U, U>L, L>A, A>W, W>S");
    int n = 0;
    for (size_t i = 4; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9' || n > 100) return bad("bad step number");
        n = n * 10 + (id[i] - '0');
    }
    if (n < 1 || n > steps) return bad("step must be 1.." + std::to_string(steps));
    s.step = n;
    s.t = float(n) / float(steps + 1);
    out = s;
    return true;
}

std::vector<StageSpec> allStages(int steps) {
    std::vector<StageSpec> v;
    const char order[5] = {'S', 'U', 'L', 'A', 'W'};
    for (char l : order) {
        StageSpec p;
        p.from = p.to = l;
        p.steps = steps;
        v.push_back(p);
        for (int k = 1; k <= steps; ++k) {
            StageSpec t;
            t.from = l;
            t.to = nextLook(l);
            t.step = k;
            t.steps = steps;
            t.t = float(k) / float(steps + 1);
            v.push_back(t);
        }
    }
    return v;
}

EffectiveStage effectiveStage(const Item& item, const StageSpec& st) {
    EffectiveStage e;
    e.from = item.hasLook(st.from) ? st.from : 'U';
    e.to = st.plateau() ? e.from : (item.hasLook(st.to) ? st.to : 'U');
    e.t = st.plateau() ? 0.0f : st.t;
    if (e.from == e.to) e.t = 0.0f;  // e.g. A>W on a texture with neither look stays vanilla
    return e;
}

}  // namespace qs::gen
