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
//   instant_switch = 0
//   no_music = 0
//   no_death_bullets = 0
//   english = 0
//
// graphics.txt holds the GRAPHICS page's settings, which change only the picture:
//
//   scale = 2
//   bloom = 0
//   anisotropy = 1
//   fullscreen = 0
//   tate = auto
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
    bool instant_switch = false;  // the color changes on the switch's press (game_tweaks.cpp)
    bool no_music = false;        // the music's sequences play silently, the effects as they are
    bool no_death_bullets = false; // a destroyed enemy fires no bullets (enemyDeathBullets)
    bool english = false;         // the game's Japanese lines in English (text.cpp)

    static Tweaks parse(const std::string& text);
    std::string text() const;
    // Each switch that is on here or in `other`.
    Tweaks either(const Tweaks& other) const;
};

struct Graphics {
    static constexpr unsigned kMaxScale = 6, kMaxBloom = 2, kMaxAnisotropy = 16;
    unsigned scale = 2;       // the render's size: 640x480 times this, 1 to kMaxScale
    unsigned bloom = 0;       // 0 off, 1 low, 2 high
    unsigned anisotropy = 1;  // the samples of the anisotropic filtering: 1 (off), 2, 4, 8 or 16
    bool fullscreen = false;
    // How the picture turns for a monitor stood on its side ("tate"). The game's VERTICAL SCREEN
    // option draws the play area sideways in the 640x480 frame, as the Dreamcast's players turned
    // their television to read it. The picture turns back only when the game is on that option.
    //   Auto   a window that is taller than wide (a rotated monitor) turns it; else it stays
    //   Off    never
    //   Left   always, a quarter turn counter-clockwise, which makes the sideways play area upright
    //   Right  always, a quarter turn clockwise, for a picture drawn the other way up
    enum Tate : unsigned { kTateAuto, kTateOff, kTateLeft, kTateRight, kTateCount };
    unsigned tate = kTateAuto;
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
