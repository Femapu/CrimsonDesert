// QuattroStagioni - F5 menu UI (internal to the overlay module).
//
// Pure Dear ImGui code: no D3D, no hooks. overlay.cpp owns the ImGui context and calls these from the
// render thread between ImGui::NewFrame() and ImGui::Render(). The menu reads qs::shared() under its
// mutex once per frame, edits a local copy and writes it back (setting configDirty) only on change.
#pragma once

struct ImFont;

namespace qs::menu {

// Loads the UI fonts (system Segoe UI when available, ImGui default otherwise). Call once after
// ImGui::CreateContext(), before the first frame.
void loadFonts();
// (Re)applies the seasonal style for a UI scale factor (1.0 = 1080p).
void applyStyle(float scale);
// Draws the menu window. Sets *open = false when the user closes it.
void draw(bool* open, float fade);
// Small hint shown for a few seconds after the overlay starts ("F5 per il menu").
void drawToast(float alpha, const char* keyName);

}  // namespace qs::menu
