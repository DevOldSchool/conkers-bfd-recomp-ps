// The pause menu's background in widescreen: the paused scene at its full width.
// Native helper called from the [[patches.hook]] snippet in config/us.toml (func_151D61B0).
//
// When the pause menu (func_150198FC) opens it copies the last frame into the Z-buffer with
// func_151D5E90, a whole-frame copy drawn as texture rectangles, then blurs that copy on the CPU
// with func_151D61B0 a little more each frame for its first few frames, and draws it as the
// background every frame with func_151D5E90 again.
//
// RT64 renders the copy from its own frame, at the full width of a widescreen picture, and draws
// the background from that while it lasts. But the CPU blur changes the copy in RDRAM, where it is
// only the 292x216 4:3 middle of the frame, so RT64 drops its own copy and the background could
// only be that middle, stretched or with nothing at the sides. In widescreen the blur is skipped,
// so the background is the paused scene itself, sharp, at its full width and its own shape. In 4:3
// the game blurs it as on an N64.
#include <cstdint>

#include "recomp.h"
#include "platform/display_mode.hpp"

// At func_151D61B0's entry: nonzero to return without blurring.
extern "C" int conker_widescreen_skip_blur() {
    return conker::display::widescreen(conker::display::current()) ? 1 : 0;
}
