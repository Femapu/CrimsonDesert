// QuattroStagioni - season tick: drives the season engine inside the plugin.
//
// A worker thread wakes every 500 ms: applies menu edits (configDirty), advances the day counter
// (playtime, manual, or the game clock once probes find it), updates qs::shared().season and passes
// the day to io::setSeasonDay, which serves every texture from the pack of its own stage (region and
// per-asset jitter) and keeps the needed packs open. The day counter is persisted in
// bin64\scripts\QuattroStagioni\state.ini because the game clock resets on load.
#pragma once
#include <string>

namespace qs {

// Reads [Seasons]/[Region.*] from the main ini (defaults when missing) and starts the thread.
// `packsActive`: io_hooks packs are running, so the tick drives io::setSeasonDay.
bool seasonStart(const std::wstring& scriptsDir, bool packsActive);
void seasonStop();

// Writes the live season config back into QuattroStagioni.ini (replacing [Seasons]/[Region.*],
// keeping every other section) and the day counter into state.ini. Used by the F5 "Salva" button.
bool seasonSave(std::string& error);

}  // namespace qs
