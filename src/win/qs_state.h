// QuattroStagioni - state shared between plugin modules (season engine, io_hooks, overlay, probes).
//
// Contract (docs/dev/CONTRACTS.md, owner: integration). One global instance, guarded by `mutex`.
// Writers: the season tick (state, activeStage), the F5 menu (config, overrides).
// Readers: io_hooks (which pack serves reads), GPU swap, game hooks (weather/light/day length).
#pragma once
#include <atomic>
#include <mutex>
#include <string>

#include "season/season.h"

namespace qs {

struct SharedState {
    std::mutex mutex;
    season::SeasonConfig config;          // live config (menu edits, saved to QuattroStagioni.ini)
    season::SeasonState season;           // last engine output
    std::atomic<bool> configDirty{false};  // set by the menu, consumed by the season tick
    std::atomic<bool> menuOpen{false};
    std::atomic<bool> gameClockHooked{false};  // true once probes found the game clock
    std::string statusLine;               // short text shown in the menu footer
};

SharedState& shared();  // defined in src/win/qs_state.cpp

}  // namespace qs
