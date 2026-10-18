// Text screen shown before the game's renderer starts: missing data files and
// the first-time MIDI preparation.  Uses libvita2d (already linked for the
// renderer) and the bundled Noto Sans JP.  It leaves libvita2d initialized
// with the renderer's own pool size, so the renderer's vita2d_init is a no-op.
#pragma once

namespace bootscreen
{
constexpr unsigned kTempPoolBytes = 8 * 1024 * 1024; // same as touhou-modern-vita GxmRenderer

// Draws the lines (UTF-8) as one frame. Initializes libvita2d on first use.
// Returns false (and only logs) if the screen cannot be shown.
bool Show(const char *const *lines, unsigned count);
// Waits until START is pressed (after it was released once).
void WaitStart();
// Frees the font; libvita2d stays initialized for the renderer.
void Finish();
} // namespace bootscreen
