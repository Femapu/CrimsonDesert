// QuattroStagioni - visual stage ids used by the generator and the pack files.
//
// Same naming as season::VisualStage::id() (DESIGN.md section 3): a plateau is one look letter
// ("A"), a transition stage is "<from>><to>.<step>" ("A>W.2") with t = step / (steps + 1).
// Cycle: S -> U -> L -> A -> W -> S. '>' is not allowed in Windows file names, so pack files use
// fileName() ("A-W.2.qsp").
#pragma once
#include <string>
#include <vector>

namespace qs::gen {

struct Item;

struct StageSpec {
    char from = 'U', to = 'U';
    int step = 0;      // 0 = plateau
    int steps = 3;     // transition quality N
    float t = 0.0f;    // step / (steps + 1)
    bool plateau() const { return step == 0; }
    std::string id() const;        // "A", "A>W.2"
    std::string fileName() const;  // "A.qsp", "A-W.2.qsp"
};

// Accepts "A", "A>W.2" and the file form "A-W.2" (optionally with ".qsp").
bool parseStage(const std::string& id, int steps, StageSpec& out, std::string* error = nullptr);
std::vector<StageSpec> allStages(int steps);  // calendar order, starting at S
char nextLook(char look);                      // S->U->L->A->W->S

// Stage as seen by one texture: looks the texture does not have (Item::looks) behave like summer.
struct EffectiveStage {
    char from = 'U', to = 'U';
    float t = 0.0f;
    bool vanilla() const { return from == 'U' && to == 'U'; }
};
EffectiveStage effectiveStage(const Item& item, const StageSpec& st);

}  // namespace qs::gen
