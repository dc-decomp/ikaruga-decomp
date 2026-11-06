// The menu that Escape, F1 or a gamepad's back button opens. The game stops while it is open. Its
// main page leads to the controller bindings (each player's keys and gamepad, the dead zone and
// the trigger ramp), to the graphics, to the mods and tweaks, and to quitting. It works on copies of
// the bindings, the graphics and the tweaks, which the window takes back, and saves, when the menu
// closes. The window shows the copy of the graphics while the menu is open.
#pragma once

#include "render/controls.h"
#include "render/overlay.h"
#include "render/settings.h"

namespace render {

class Menu {
public:
    enum class Key { Up, Down, Left, Right, Accept, Back, Clear };
    // What a key did: nothing for the window, the menu closed, or the player chose QUIT.
    enum class Result { Stay, Closed, Quit };

    bool is_open() const { return open_; }
    void open(const Bindings& b, const Graphics& g, const Tweaks& t);
    Result key(Key k);
    // Capturing an input for the chosen row: the window gives it the next key or gamepad input of
    // the chosen device, or cancels with Back.
    bool capturing() const { return capturing_; }
    bool for_keyboard() const { return keyboard_; }
    void capture(const Binding& b);
    // The mouse, in the window's pixels as draw() takes them, on the rows where the last draw() put
    // them: the pointer over a row chooses it, and a click acts as Return, or on a setting's value as
    // Left or Right by the half it hits. The right button acts as Escape.
    void point(float x, float y);
    Result click(float x, float y, bool right);
    const Bindings& bindings() const { return bindings_; }
    const Graphics& graphics() const { return graphics_; }
    // The window's own fullscreen state, which its green button and Control-Command-F change too.
    void set_fullscreen(bool on) { graphics_.fullscreen = on; }
    // The settings a command line overrode, so the menu starts from them.
    void set_graphics(const Graphics& g) { graphics_ = g; }
    const Tweaks& tweaks() const { return tweaks_; }
    bool bindings_changed() const { return bindings_changed_; }
    bool graphics_changed() const { return graphics_changed_; }
    bool tweaks_changed() const { return tweaks_changed_; }
    void draw(Overlay& o, uint32_t width, uint32_t height) const;

private:
    enum class Page { Main, Controller, Graphics, Tweaks };
    // The main page's rows.
    static constexpr int kResume = 0, kController = 1, kGraphicsPage = 2, kTweaksPage = 3, kQuit = 4, kMainRows = 5;
    // The controller page's rows: the player, the device, the controls, the dead zone, the trigger
    // ramp, the mouse for the keyboard or the gamepad's choice for the gamepad, and DONE.
    static constexpr int kPlayer = 0, kDevice = 1, kFirstControl = 2, kDeadzone = kFirstControl + kPadControls,
                         kRamp = kDeadzone + 1, kMouseOrPad = kRamp + 1, kDone = kMouseOrPad + 1,
                         kControllerRows = kDone + 1;
    // The graphics page's rows: the resolution, the bloom, the anisotropic filtering, fullscreen,
    // the monitor's rotation (tate), vsync, Metal's HUD, and BACK. Only Metal has a HUD, so the row
    // is on macOS only: elsewhere kHud matches no row.
    static constexpr int kScale = 0, kBloom = 1, kAnisotropy = 2, kFullscreen = 3, kTate = 4, kVsync = 5,
#ifdef __APPLE__
                         kHud = 6, kGraphicsBack = 7, kGraphicsRows = 8;
#else
                         kHud = -1, kGraphicsBack = 6, kGraphicsRows = 7;
#endif
    // The tweaks page's rows: one for each switch (kTweakRows in menu.cpp), and BACK.
    static constexpr int kTweaksBack = 8, kTweaksRows = 9;

    int rows() const;
    // The row at a point of the last draw(), or -1, and whether a row's value steps with Left and
    // Right.
    int row_at(float x, float y) const;
    bool steps(int row) const;
    Result controller_key(Key k);
    Bindings::Section& section() { return (keyboard_ ? bindings_.keyboard : bindings_.gamepad)[player_]; }
    // The mouse is player 1's, on the keyboard.
    bool mouse_row() const { return keyboard_ && player_ == 0; }
    void step_mouse_or_pad();
    Result graphics_key(Key k);
    Result tweaks_key(Key k);
    void draw_controller(Overlay& o, float x0, float y0, float panel_w, float unit) const;

    // Where the last draw() put the panel: its corner, width and line height, and its values' column.
    mutable float x0_ = 0, y0_ = 0, width_ = 0, unit_ = 0, value_x_ = 0;
    Bindings bindings_;
    Graphics graphics_;
    Tweaks tweaks_;
    Page page_ = Page::Main;
    int row_ = 0;
    unsigned player_ = 0;
    bool open_ = false, keyboard_ = true, capturing_ = false;
    bool bindings_changed_ = false, graphics_changed_ = false, tweaks_changed_ = false;
};

}  // namespace render
