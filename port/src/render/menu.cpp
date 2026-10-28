#include "render/menu.h"

#include <algorithm>
#include <cctype>

namespace render {
namespace {

// A binding as the screen shows it: the key's name, or the pad's button or axis.
std::string shown(const Binding& b) {
    std::string t = b.text();
    if (t.rfind("key:", 0) == 0)
        return t.substr(4);
    if (t.rfind("button:", 0) == 0)
        t = "PAD " + t.substr(7);
    else if (t.rfind("axis:", 0) == 0)
        t = "PAD " + t.substr(5);
    else
        return "-";
    for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return t;
}

const char* on_off(bool on) { return on ? "< ON >" : "< OFF >"; }

// The tweaks page's switches in their order, each with one or two lines about it.
struct TweakRow {
    const char* label;
    bool Tweaks::*value;
    const char* help;
};
constexpr TweakRow kTweakRows[] = {
    {"INFINITE LIVES", &Tweaks::infinite_lives, "A PLAYER IN THE GAME KEEPS AT LEAST 2 LIVES."},
    {"INFINITE LASER CHARGE", &Tweaks::infinite_charge, "THE HOMING LASERS' ENERGY STAYS FULL."},
    {"HALF THE BULLETS", &Tweaks::half_bullets, "EVERY SECOND BULLET ENDS AS IT APPEARS."},
    {"HIDE PRESS START", &Tweaks::no_press_start, "NO BLINKING PRESS START ON A FREE PLAYER'S SIDE."},
    {"SWITCH AT ONCE", &Tweaks::instant_switch,
     "THE COLOR CHANGES ON THE PRESS, NOT HALFWAY THROUGH THE TURN (7 FRAMES).\n"
     "THE TURNING SHIP SHOOTS, CAN SWITCH BACK, AND IS NEVER OPEN TO BOTH COLORS."},
    {"NO MUSIC", &Tweaks::no_music, "THE MUSIC IS SILENT, THE SOUND EFFECTS PLAY."},
    {"NO DEATH BULLETS", &Tweaks::no_death_bullets,
     "A DESTROYED ENEMY FIRES NO BULLETS BACK. THE GAME FIRES THEM ON NORMAL AFTER\n"
     "A KILL WITH THE ENEMY'S OWN COLOR, AND ON HARD AFTER EVERY KILL."},
    {"ENGLISH TEXT", &Tweaks::english,
     "THE GAME'S JAPANESE IN ENGLISH: THE MENUS AND THEIR HELP, THE MESSAGES,\n"
     "THE MOTTO, THE CHAPTERS' CARDS, THE LAST CHAPTER'S WORDS AND THE ENDING."},
};

// One step of a setting from `low` to `high`: Left and Right stop at the ends, and Return goes
// round.
unsigned stepped(unsigned value, unsigned low, unsigned high, Menu::Key k) {
    if (k == Menu::Key::Left)
        return value > low ? value - 1 : value;
    if (k == Menu::Key::Right)
        return value < high ? value + 1 : value;
    return value < high ? value + 1 : low;
}

}  // namespace

void Menu::open(const Bindings& b, const Graphics& g, const Tweaks& t) {
    bindings_ = b;
    graphics_ = g;
    tweaks_ = t;
    open_ = true;
    page_ = Page::Main;
    row_ = kResume;
    player_ = 0;
    capturing_ = false;
    bindings_changed_ = false;
    graphics_changed_ = false;
    tweaks_changed_ = false;
}

int Menu::rows() const {
    switch (page_) {
        case Page::Main: return kMainRows;
        case Page::Controller: return kControllerRows;
        case Page::Graphics: return kGraphicsRows;
        case Page::Tweaks: return kTweaksRows;
    }
    return 1;
}

Menu::Result Menu::key(Key k) {
    if (capturing_) {
        if (k == Key::Back)
            capturing_ = false;
        return Result::Stay;
    }
    if (k == Key::Up || k == Key::Down) {
        row_ = (row_ + (k == Key::Up ? rows() - 1 : 1)) % rows();
        return Result::Stay;
    }
    if (page_ == Page::Controller)
        return controller_key(k);
    if (page_ == Page::Graphics)
        return graphics_key(k);
    if (page_ == Page::Tweaks)
        return tweaks_key(k);
    if (k == Key::Back || (k == Key::Accept && row_ == kResume)) {
        open_ = false;
        return Result::Closed;
    }
    if (k == Key::Accept && row_ == kQuit) {
        open_ = false;
        return Result::Quit;
    }
    if (k == Key::Accept && row_ == kController) {
        page_ = Page::Controller;
        row_ = kFirstControl;
    }
    else if (k == Key::Accept && row_ == kGraphicsPage) {
        page_ = Page::Graphics;
        row_ = kScale;
    }
    else if (k == Key::Accept && row_ == kTweaksPage) {
        page_ = Page::Tweaks;
        row_ = 0;
    }
    return Result::Stay;
}

void Menu::step_mouse_or_pad() {
    if (mouse_row())
        bindings_.mouse = !bindings_.mouse;
    else if (!keyboard_)
        bindings_.pad[player_] = 1 - bindings_.pad[player_];
    else
        return;
    bindings_changed_ = true;
}

Menu::Result Menu::controller_key(Key k) {
    switch (k) {
        case Key::Left:
        case Key::Right: {
            const int step = k == Key::Left ? -1 : 1;
            if (row_ == kPlayer)
                player_ = 1 - player_;
            else if (row_ == kDevice)
                keyboard_ = !keyboard_;
            else if (row_ == kDeadzone) {
                bindings_.deadzone_percent = std::clamp(bindings_.deadzone_percent + 5 * step, 0, 90);
                bindings_changed_ = true;
            }
            else if (row_ == kRamp) {
                bindings_.trigger_ramp = !bindings_.trigger_ramp;
                bindings_changed_ = true;
            }
            else if (row_ == kMouseOrPad)
                step_mouse_or_pad();
            break;
        }
        case Key::Accept:
            if (row_ == kDone) {
                page_ = Page::Main;
                row_ = kController;
            }
            else if (row_ == kPlayer)
                player_ = 1 - player_;
            else if (row_ == kDevice)
                keyboard_ = !keyboard_;
            else if (row_ == kRamp) {
                bindings_.trigger_ramp = !bindings_.trigger_ramp;
                bindings_changed_ = true;
            }
            else if (row_ == kMouseOrPad)
                step_mouse_or_pad();
            else if (row_ >= kFirstControl && row_ < kDeadzone)
                capturing_ = true;
            break;
        case Key::Clear:
            if (row_ >= kFirstControl && row_ < kDeadzone) {
                section()[static_cast<unsigned>(row_ - kFirstControl)] = Binding{};
                bindings_changed_ = true;
            }
            break;
        case Key::Back:
            page_ = Page::Main;
            row_ = kController;
            break;
        default:
            break;
    }
    return Result::Stay;
}

Menu::Result Menu::graphics_key(Key k) {
    if (k == Key::Back || (k == Key::Accept && row_ == kGraphicsBack)) {
        page_ = Page::Main;
        row_ = kGraphicsPage;
        return Result::Stay;
    }
    if (k != Key::Accept && k != Key::Left && k != Key::Right)
        return Result::Stay;
    const Graphics before = graphics_;
    if (row_ == kScale)
        graphics_.scale = stepped(graphics_.scale, 1, Graphics::kMaxScale, k);
    else if (row_ == kBloom)
        graphics_.bloom = stepped(graphics_.bloom, 0, Graphics::kMaxBloom, k);
    else if (row_ == kAnisotropy) {
        // In powers of two: 1 is off.
        unsigned power = 0;
        while ((2u << power) <= graphics_.anisotropy) ++power;
        graphics_.anisotropy = 1u << stepped(power, 0, 4, k);
    }
    else if (row_ == kFullscreen)
        graphics_.fullscreen = !graphics_.fullscreen;
    else if (row_ == kVsync)
        graphics_.vsync = !graphics_.vsync;
    else if (row_ == kHud)
        graphics_.hud = !graphics_.hud;
    graphics_changed_ |= graphics_.scale != before.scale || graphics_.bloom != before.bloom ||
                         graphics_.anisotropy != before.anisotropy || graphics_.fullscreen != before.fullscreen ||
                         graphics_.vsync != before.vsync || graphics_.hud != before.hud;
    return Result::Stay;
}

Menu::Result Menu::tweaks_key(Key k) {
    static_assert(sizeof kTweakRows / sizeof kTweakRows[0] == kTweaksBack);
    if (k == Key::Back || (k == Key::Accept && row_ == kTweaksBack)) {
        page_ = Page::Main;
        row_ = kTweaksPage;
        return Result::Stay;
    }
    if (k != Key::Accept && k != Key::Left && k != Key::Right)
        return Result::Stay;
    if (row_ < kTweaksBack) {
        bool& value = tweaks_.*kTweakRows[row_].value;
        value = !value;
        tweaks_changed_ = true;
    }
    return Result::Stay;
}

int Menu::row_at(float x, float y) const {
    if (unit_ <= 0 || x < x0_ + unit_ * 0.5f || x > x0_ + width_ - unit_ * 0.5f)
        return -1;
    for (int r = 0; r < rows(); ++r) {
        const float top = y0_ + unit_ * (3.5f + static_cast<float>(r));
        if (y >= top && y < top + unit_)
            return r;
    }
    return -1;
}

bool Menu::steps(int row) const {
    switch (page_) {
        case Page::Main: return false;
        case Page::Controller:
            return row == kPlayer || row == kDevice || row == kDeadzone || row == kRamp ||
                   (row == kMouseOrPad && (mouse_row() || !keyboard_));
        case Page::Graphics: return row != kGraphicsBack;
        case Page::Tweaks: return row != kTweaksBack;
    }
    return false;
}

void Menu::point(float x, float y) {
    if (const int r = row_at(x, y); r >= 0 && !capturing_)
        row_ = r;
}

Menu::Result Menu::click(float x, float y, bool right) {
    if (right)
        return key(Key::Back);
    const int r = row_at(x, y);
    if (r < 0 || capturing_)
        return Result::Stay;
    row_ = r;
    if (steps(r) && x >= value_x_)
        return key(x < value_x_ + (x0_ + width_ - value_x_) / 2 ? Key::Left : Key::Right);
    return key(Key::Accept);
}

void Menu::capture(const Binding& b) {
    if (!capturing_)
        return;
    section()[static_cast<unsigned>(row_ - kFirstControl)] = b;
    capturing_ = false;
    bindings_changed_ = true;
}

// A panel in the middle of the window, with one line for each row and the chosen one marked.
void Menu::draw(Overlay& o, uint32_t width, uint32_t height) const {
    const float unit = std::max(12.0f, static_cast<float>(height) / 40.0f);  // one line's height
    const float panel_w = std::min(static_cast<float>(width) * 0.9f, unit * 30.0f);
    const bool graphics = page_ == Page::Graphics;
    const bool notes = graphics || page_ == Page::Tweaks;  // two lines under the rows
    const float panel_h = unit * (static_cast<float>(rows()) + (notes ? 6.4f : 4.4f));
    const float x0 = (static_cast<float>(width) - panel_w) / 2, y0 = (static_cast<float>(height) - panel_h) / 2;
    o.box(x0, y0, panel_w, panel_h, 0xE0101010u);
    x0_ = x0;
    y0_ = y0;
    width_ = panel_w;
    unit_ = unit;
    value_x_ = x0 + panel_w * (page_ == Page::Controller ? 0.5f : 0.6f);
    if (page_ == Page::Controller) {
        draw_controller(o, x0, y0, panel_w, unit);
        return;
    }
    const float text = unit * 0.66f;
    const bool main = page_ == Page::Main;
    o.text(x0 + unit, y0 + unit * 0.7f, main ? "MENU" : graphics ? "GRAPHICS" : "MODS AND TWEAKS", unit * 0.8f,
           0xFFFFFFFFu);
    o.text(x0 + unit, y0 + unit * 1.9f,
           main ? "ARROWS OR MOUSE CHOOSE  -  RETURN OR CLICK SELECTS  -  ESC RESUMES"
                : "ARROWS OR MOUSE CHOOSE  -  RETURN OR CLICK CHANGES  -  ESC GOES BACK",
           text * 0.8f, 0xFFA0A0A0u);
    for (int r = 0; r < rows(); ++r) {
        const float y = y0 + unit * (3.6f + static_cast<float>(r));
        const bool chosen = r == row_;
        if (chosen)
            o.box(x0 + unit * 0.5f, y - unit * 0.1f, panel_w - unit, unit, 0xFF404040u);
        std::string label, value;
        if (main) {
            static const char* const kLabels[kMainRows] = {"RESUME", "CONTROLLER", "GRAPHICS", "MODS AND TWEAKS",
                                                           "QUIT"};
            label = kLabels[r];
        }
        else if (graphics && r == kScale) {
            label = "RESOLUTION";
            value = "< " + std::to_string(graphics_.scale) + "X  " + std::to_string(640 * graphics_.scale) + "X" +
                    std::to_string(480 * graphics_.scale) + " >";
        }
        else if (graphics && r == kBloom) {
            static const char* const kBloomNames[Graphics::kMaxBloom + 1] = {"< OFF >", "< LOW >", "< HIGH >"};
            label = "BLOOM";
            value = kBloomNames[graphics_.bloom];
        }
        else if (graphics && r == kAnisotropy) {
            label = "ANISOTROPIC FILTERING";
            value = graphics_.anisotropy > 1 ? "< " + std::to_string(graphics_.anisotropy) + "X >" : "< OFF >";
        }
        else if (graphics && r == kFullscreen) {
            label = "FULLSCREEN";
            value = on_off(graphics_.fullscreen);
        }
        else if (graphics && r == kVsync) {
            label = "VSYNC";
            value = on_off(graphics_.vsync);
        }
        else if (graphics && r == kHud) {
            label = "METAL HUD";
            value = on_off(graphics_.hud);
        }
        else if (graphics) {
            label = "BACK";
        }
        else if (r < kTweaksBack) {
            label = kTweakRows[r].label;
            value = on_off(tweaks_.*kTweakRows[r].value);
        }
        else {
            label = "BACK";
        }
        const uint32_t c = chosen ? 0xFFFFFFFFu : 0xFFC8C8C8u;
        o.text(x0 + unit, y, label, text, c);
        if (!value.empty())
            o.text(x0 + panel_w * 0.6f, y, value, text, c);
    }
    if (graphics) {
        // The 4:3 picture in the window, which a larger resolution is averaged down to.
        const uint32_t shown = std::min(height, width * 3 / 4);
        const std::string note = "IN THE WINDOW: " + std::to_string(shown * 4 / 3) + "X" + std::to_string(shown) +
                                 (480 * graphics_.scale > shown    ? ", SUPERSAMPLED"
                                  : 480 * graphics_.scale == shown ? ", ONE TO ONE"
                                                                   : ", SCALED UP");
        o.text(x0 + unit, y0 + unit * (4.0f + static_cast<float>(rows())), note, text * 0.8f, 0xFFA0A0A0u);
        o.text(x0 + unit, y0 + unit * (4.8f + static_cast<float>(rows())),
               "VSYNC OFF: LESS LATENCY IN FULLSCREEN, BUT THE PICTURE CAN TEAR", text * 0.8f, 0xFFA0A0A0u);
    }
    else if (page_ == Page::Tweaks && row_ < kTweaksBack) {
        const std::string help = kTweakRows[row_].help;
        const size_t cut = help.find('\n');
        o.text(x0 + unit, y0 + unit * (4.0f + static_cast<float>(rows())), help.substr(0, cut), text * 0.8f,
               0xFFA0A0A0u);
        if (cut != std::string::npos)
            o.text(x0 + unit, y0 + unit * (4.8f + static_cast<float>(rows())), help.substr(cut + 1), text * 0.8f,
                   0xFFA0A0A0u);
    }
}

void Menu::draw_controller(Overlay& o, float x0, float y0, float panel_w, float unit) const {
    const float text = unit * 0.66f;
    o.text(x0 + unit, y0 + unit * 0.7f, "CONTROLLER", unit * 0.8f, 0xFFFFFFFFu);
    const char* hint = capturing_ ? (keyboard_ ? "PRESS A KEY  -  ESC CANCELS" : "PRESS A BUTTON OR MOVE A STICK  -  ESC CANCELS")
                                  : "ARROWS CHOOSE  -  RETURN OR CLICK CHANGES  -  BACKSPACE CLEARS  -  ESC GOES BACK";
    o.text(x0 + unit, y0 + unit * 1.9f, hint, text * 0.8f, 0xFFA0A0A0u);
    const auto& section = (keyboard_ ? bindings_.keyboard : bindings_.gamepad)[player_];
    for (int r = 0; r < kControllerRows; ++r) {
        const float y = y0 + unit * (3.6f + static_cast<float>(r));
        const bool chosen = r == row_;
        if (chosen)
            o.box(x0 + unit * 0.5f, y - unit * 0.1f, panel_w - unit, unit, capturing_ ? 0xFF5050A0u : 0xFF404040u);
        std::string label, value;
        bool usable = true;
        if (r == kPlayer) {
            label = "PLAYER";
            value = player_ == 0 ? "< 1 >" : "< 2 >";
        }
        else if (r == kDevice) {
            label = "DEVICE";
            value = keyboard_ ? "< KEYBOARD >" : "< GAMEPAD >";
        }
        else if (r < kDeadzone) {
            const Pad c = static_cast<Pad>(r - kFirstControl);
            label = pad_label(c);
            const Binding& b = section[static_cast<unsigned>(c)];
            value = capturing_ && chosen ? "..." : shown(b);
        }
        else if (r == kDeadzone) {
            label = "STICK DEAD ZONE";
            value = "< " + std::to_string(bindings_.deadzone_percent) + "% >";
        }
        else if (r == kRamp) {
            label = "TRIGGER RAMP";
            value = bindings_.trigger_ramp ? "< ON >" : "< OFF >";
        }
        else if (r == kMouseOrPad && !keyboard_) {
            label = "GAMEPAD";
            value = bindings_.pad[player_] == 0 ? "< FIRST PLUGGED IN >" : "< SECOND PLUGGED IN >";
        }
        else if (r == kMouseOrPad) {
            label = "MOUSE STEERS THE SHIP";
            value = player_ == 0 ? (bindings_.mouse ? "< ON >" : "< OFF >") : "PLAYER 1 ONLY";
            usable = player_ == 0;
        }
        else {
            label = "DONE";
        }
        const uint32_t c = !usable ? 0xFF707070u : chosen ? 0xFFFFFFFFu : 0xFFC8C8C8u;
        o.text(x0 + unit, y, label, text, c);
        if (!value.empty())
            o.text(x0 + panel_w * 0.5f, y, value, text, c);
    }
}

}  // namespace render
