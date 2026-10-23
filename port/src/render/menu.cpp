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

}  // namespace

void Menu::open(const Bindings& b) {
    bindings_ = b;
    open_ = true;
    capturing_ = false;
    changed_ = false;
}

bool Menu::key(Key k) {
    if (capturing_) {
        if (k == Key::Back)
            capturing_ = false;
        return false;
    }
    switch (k) {
        case Key::Up: row_ = (row_ + kRows - 1) % kRows; break;
        case Key::Down: row_ = (row_ + 1) % kRows; break;
        case Key::Left:
        case Key::Right: {
            const int step = k == Key::Left ? -1 : 1;
            if (row_ == kDevice)
                keyboard_ = !keyboard_;
            else if (row_ == kDeadzone) {
                bindings_.deadzone_percent = std::clamp(bindings_.deadzone_percent + 5 * step, 0, 90);
                changed_ = true;
            }
            else if (row_ == kRamp) {
                bindings_.trigger_ramp = !bindings_.trigger_ramp;
                changed_ = true;
            }
            break;
        }
        case Key::Accept:
            if (row_ == kDone) {
                open_ = false;
                return true;
            }
            if (row_ == kDevice)
                keyboard_ = !keyboard_;
            else if (row_ == kRamp) {
                bindings_.trigger_ramp = !bindings_.trigger_ramp;
                changed_ = true;
            }
            else if (row_ >= kFirstControl && row_ < kDeadzone)
                capturing_ = true;
            break;
        case Key::Clear:
            if (row_ >= kFirstControl && row_ < kDeadzone) {
                (keyboard_ ? bindings_.keyboard : bindings_.gamepad)[row_ - kFirstControl] = Binding{};
                changed_ = true;
            }
            break;
        case Key::Back:
            open_ = false;
            return true;
    }
    return false;
}

void Menu::capture(const Binding& b) {
    if (!capturing_)
        return;
    (keyboard_ ? bindings_.keyboard : bindings_.gamepad)[row_ - kFirstControl] = b;
    capturing_ = false;
    changed_ = true;
}

// A panel in the middle of the window, with one line for each row and the chosen one marked.
void Menu::draw(Overlay& o, uint32_t width, uint32_t height) const {
    const float unit = std::max(10.0f, static_cast<float>(height) / 48.0f);  // one line's height
    const float panel_w = std::min(static_cast<float>(width) * 0.9f, unit * 30.0f);
    const float panel_h = unit * (kRows + 4.4f);
    const float x0 = (static_cast<float>(width) - panel_w) / 2, y0 = (static_cast<float>(height) - panel_h) / 2;
    const float text = unit * 0.62f;
    o.box(x0, y0, panel_w, panel_h, 0xE0101010u);
    o.text(x0 + unit, y0 + unit * 0.7f, "CONTROLLER", unit * 0.8f, 0xFFFFFFFFu);
    const char* hint = capturing_ ? (keyboard_ ? "PRESS A KEY  -  ESC CANCELS" : "PRESS A BUTTON OR MOVE A STICK  -  ESC CANCELS")
                                  : "ARROWS CHOOSE  -  RETURN CHANGES  -  BACKSPACE CLEARS  -  ESC CLOSES";
    o.text(x0 + unit, y0 + unit * 1.9f, hint, text * 0.8f, 0xFFA0A0A0u);
    const auto& section = keyboard_ ? bindings_.keyboard : bindings_.gamepad;
    for (int r = 0; r < kRows; ++r) {
        const float y = y0 + unit * (3.6f + r);
        const bool chosen = r == row_;
        if (chosen)
            o.box(x0 + unit * 0.5f, y - unit * 0.1f, panel_w - unit, unit, capturing_ ? 0xFF5050A0u : 0xFF404040u);
        std::string label, value;
        if (r == kDevice) {
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
        else {
            label = "DONE";
        }
        const uint32_t c = chosen ? 0xFFFFFFFFu : 0xFFC8C8C8u;
        o.text(x0 + unit, y, label, text, c);
        if (!value.empty())
            o.text(x0 + panel_w * 0.5f, y, value, text, c);
    }
}

}  // namespace render
