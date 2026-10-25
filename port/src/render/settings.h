// The menu's settings that live in files next to the bindings, as `key = value` lines. Unknown lines
// are skipped, and the defaults fill what a file leaves.
//
// tweaks.txt holds the switches of the MODS AND TWEAKS page. The game loop applies them to the
// game's memory at each vertical blank (src/game_tweaks.cpp):
//
//   infinite_lives = 1
//   infinite_charge = 0
//   half_bullets = 0
//   no_press_start = 0
//
// graphics.txt holds the GRAPHICS page's settings, which change only the picture:
//
//   scale = 2
//   bloom = 0
//   anisotropy = 1
//   fullscreen = 0
//   vsync = 0
//   hud = 0
#pragma once

#include <string>

namespace render {

struct Tweaks {
    bool infinite_lives = false;  // a player in the game keeps at least 2 lives
    bool infinite_charge = false; // the homing lasers' energy stays full
    bool half_bullets = false;    // every second bullet ends as it appears
    bool no_press_start = false;  // no blinking PRESS START on a free player's side in play

    static Tweaks parse(const std::string& text);
    std::string text() const;
};

struct Graphics {
    static constexpr unsigned kMaxScale = 6, kMaxBloom = 2, kMaxAnisotropy = 16;
    unsigned scale = 2;       // the render's size: 640x480 times this, 1 to kMaxScale
    unsigned bloom = 0;       // 0 off, 1 low, 2 high
    unsigned anisotropy = 1;  // the samples of the anisotropic filtering: 1 (off), 2, 4, 8 or 16
    bool fullscreen = false;
    bool vsync = false;  // off: in fullscreen, each picture goes to the screen at once, which can tear
    bool hud = false;    // Metal's performance HUD: the frame rate, the frame times and the GPU's time

    static Graphics parse(const std::string& text);
    std::string text() const;
};

// The file `name` in SDL's preference folder for this port, or empty.
std::string settings_path(const char* name);
// The file's text, empty when there is no file.
std::string read_settings(const std::string& path);
bool write_settings(const std::string& path, const std::string& text);

}  // namespace render
