#include "render/controls.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace render {
namespace {

struct Names {
    const char* key;
    const char* label;
};
constexpr Names kNames[kPadControls] = {
    {"a", "A"},           {"b", "B"},           {"x", "X"},           {"y", "Y"},
    {"start", "START"},   {"dpad_up", "D-PAD UP"}, {"dpad_down", "D-PAD DOWN"}, {"dpad_left", "D-PAD LEFT"},
    {"dpad_right", "D-PAD RIGHT"}, {"ltrigger", "LEFT TRIGGER"}, {"rtrigger", "RIGHT TRIGGER"},
    {"stick_up", "STICK UP"}, {"stick_down", "STICK DOWN"}, {"stick_left", "STICK LEFT"},
    {"stick_right", "STICK RIGHT"},
};

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

Binding key(SDL_Scancode s) { return {Binding::Source::Key, s, 1}; }
Binding button(SDL_GamepadButton b) { return {Binding::Source::Button, b, 1}; }
Binding axis(SDL_GamepadAxis a, int sign) { return {Binding::Source::Axis, a, sign}; }

}  // namespace

const char* pad_key(Pad c) { return kNames[static_cast<unsigned>(c)].key; }
const char* pad_label(Pad c) { return kNames[static_cast<unsigned>(c)].label; }

std::string Binding::text() const {
    switch (source) {
        case Source::Key: return std::string("key:") + SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
        case Source::Button: {
            const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(code));
            return std::string("button:") + (n ? n : "?");
        }
        case Source::Axis: {
            const char* n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(code));
            return std::string("axis:") + (n ? n : "?") + (sign < 0 ? "-" : "+");
        }
        default: return "none";
    }
}

Binding Binding::parse(const std::string& s) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos)
        return {};
    const std::string kind = s.substr(0, colon);
    std::string name = s.substr(colon + 1);
    if (kind == "key") {
        const SDL_Scancode c = SDL_GetScancodeFromName(name.c_str());
        return c == SDL_SCANCODE_UNKNOWN ? Binding{} : key(c);
    }
    if (kind == "button") {
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
        return b == SDL_GAMEPAD_BUTTON_INVALID ? Binding{} : button(b);
    }
    if (kind == "axis" && !name.empty() && (name.back() == '+' || name.back() == '-')) {
        const int sign = name.back() == '-' ? -1 : 1;
        name.pop_back();
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name.c_str());
        return a == SDL_GAMEPAD_AXIS_INVALID ? Binding{} : axis(a, sign);
    }
    return {};
}

Bindings Bindings::defaults() {
    Bindings b;
    auto set = [&b](Pad c, Binding k, Binding g) {
        b.keyboard[static_cast<unsigned>(c)] = k;
        b.gamepad[static_cast<unsigned>(c)] = g;
    };
    set(Pad::A, key(SDL_SCANCODE_Z), button(SDL_GAMEPAD_BUTTON_SOUTH));
    set(Pad::B, key(SDL_SCANCODE_X), button(SDL_GAMEPAD_BUTTON_EAST));
    set(Pad::X, key(SDL_SCANCODE_A), button(SDL_GAMEPAD_BUTTON_WEST));
    set(Pad::Y, key(SDL_SCANCODE_S), button(SDL_GAMEPAD_BUTTON_NORTH));
    set(Pad::Start, key(SDL_SCANCODE_RETURN), button(SDL_GAMEPAD_BUTTON_START));
    set(Pad::Up, key(SDL_SCANCODE_UP), button(SDL_GAMEPAD_BUTTON_DPAD_UP));
    set(Pad::Down, key(SDL_SCANCODE_DOWN), button(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    set(Pad::Left, key(SDL_SCANCODE_LEFT), button(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
    set(Pad::Right, key(SDL_SCANCODE_RIGHT), button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
    set(Pad::LeftTrigger, key(SDL_SCANCODE_Q), axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1));
    set(Pad::RightTrigger, key(SDL_SCANCODE_W), axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1));
    // The arrows drive the stick too: a title that steers with the stick is playable from a keyboard.
    set(Pad::StickUp, key(SDL_SCANCODE_UP), axis(SDL_GAMEPAD_AXIS_LEFTY, -1));
    set(Pad::StickDown, key(SDL_SCANCODE_DOWN), axis(SDL_GAMEPAD_AXIS_LEFTY, 1));
    set(Pad::StickLeft, key(SDL_SCANCODE_LEFT), axis(SDL_GAMEPAD_AXIS_LEFTX, -1));
    set(Pad::StickRight, key(SDL_SCANCODE_RIGHT), axis(SDL_GAMEPAD_AXIS_LEFTX, 1));
    return b;
}

Bindings Bindings::parse(const std::string& text, std::string& warnings) {
    Bindings b = defaults();
    std::array<Binding, kPadControls>* section = nullptr;
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (line == "[keyboard]" || line == "[gamepad]") {
            section = line == "[keyboard]" ? &b.keyboard : &b.gamepad;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warnings += "line " + std::to_string(number) + ": no '='\n";
            continue;
        }
        const std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (!section) {
            if (name == "deadzone")
                b.deadzone_percent = std::clamp(std::atoi(value.c_str()), 0, 90);
            else if (name == "trigger_ramp")
                b.trigger_ramp = value != "0";
            else if (name != "version")
                warnings += "line " + std::to_string(number) + ": unknown setting " + name + "\n";
            continue;
        }
        unsigned i = 0;
        while (i < kPadControls && name != kNames[i].key) ++i;
        if (i == kPadControls) {
            warnings += "line " + std::to_string(number) + ": unknown control " + name + "\n";
            continue;
        }
        (*section)[i] = Binding::parse(value);
        if ((*section)[i].source == Binding::Source::None && value != "none")
            warnings += "line " + std::to_string(number) + ": no such input " + value + "\n";
    }
    return b;
}

std::string Bindings::text() const {
    std::ostringstream o;
    o << "# Ikaruga's controller bindings, rewritten when the F1 screen changes them.\n"
      << "# The inputs are SDL's own names.\n"
      << "version = 1\n"
      << "deadzone = " << deadzone_percent << "\n"
      << "trigger_ramp = " << (trigger_ramp ? 1 : 0) << "\n";
    for (const auto* section : {&keyboard, &gamepad}) {
        o << (section == &keyboard ? "\n[keyboard]\n" : "\n[gamepad]\n");
        for (unsigned i = 0; i < kPadControls; ++i) o << kNames[i].key << " = " << (*section)[i].text() << "\n";
    }
    return o.str();
}

std::string bindings_path() {
    char* dir = SDL_GetPrefPath("ikaruga", "ikaruga");
    if (!dir)
        return {};
    std::string path = std::string(dir) + "bindings.txt";
    SDL_free(dir);
    return path;
}

std::string dream_recomp_bindings_path() {
    char* dir = SDL_GetPrefPath("dream-recomp", "dream-recomp");
    if (!dir)
        return {};
    std::string path = std::string(dir) + "bindings.txt";
    SDL_free(dir);
    return path;
}

Bindings load_bindings(const std::string& path, std::string& warnings) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Bindings::defaults();
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Bindings::parse(text, warnings);
}

bool save_bindings(const Bindings& b, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    out << b.text();
    return static_cast<bool>(out);
}

void Input::update(bool enabled) {
    const uint64_t now = SDL_GetTicksNS();
    // Measured, so the ramp takes the same time on a fast host as on a slow one. A stall does not
    // jump it to full.
    const float dt = last_ns_ ? std::min(static_cast<float>(now - last_ns_) * 1e-9f, 0.25f) : 0.0f;
    last_ns_ = now;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const float dead = static_cast<float>(bindings.deadzone_percent) / 100.0f;
    for (unsigned i = 0; i < kPadControls; ++i) {
        bool digital = false;
        float analogue = 0.0f;
        if (enabled) {
            const Binding& k = bindings.keyboard[i];
            if (keys && k.source == Binding::Source::Key && keys[k.code])
                digital = true;
            const Binding& g = bindings.gamepad[i];
            for (SDL_Gamepad* p : pads) {
                if (g.source == Binding::Source::Button &&
                    SDL_GetGamepadButton(p, static_cast<SDL_GamepadButton>(g.code)))
                    digital = true;
                if (g.source == Binding::Source::Axis) {
                    // The bound half only, so left and right are two bindings on one stick, and
                    // rescaled from the dead zone's edge, so the value does not jump there.
                    const float raw = SDL_GetGamepadAxis(p, static_cast<SDL_GamepadAxis>(g.code)) / 32767.0f;
                    const float half = g.sign < 0 ? -raw : raw;
                    if (half > dead)
                        analogue = std::max(analogue, std::min(1.0f, (half - dead) / (1.0f - dead)));
                }
            }
        }
        const bool trigger = i == static_cast<unsigned>(Pad::LeftTrigger) || i == static_cast<unsigned>(Pad::RightTrigger);
        if (trigger && bindings.trigger_ramp) {
            ramp_[i] = digital ? std::min(1.0f, ramp_[i] + dt / 0.15f) : 0.0f;
            value_[i] = std::max(analogue, ramp_[i]);
        }
        else {
            ramp_[i] = 0.0f;
            value_[i] = digital ? 1.0f : analogue;
        }
    }
}

Controls Input::controls() const {
    Controls c;
    const std::pair<Pad, uint16_t> buttons[] = {
        {Pad::A, kButtonA},   {Pad::B, kButtonB},       {Pad::X, kButtonX},       {Pad::Y, kButtonY},
        {Pad::Start, kButtonStart}, {Pad::Up, kButtonUp}, {Pad::Down, kButtonDown}, {Pad::Left, kButtonLeft},
        {Pad::Right, kButtonRight},
    };
    for (const auto& [pad, bit] : buttons)
        if (value(pad) > 0.0f)
            c.held |= bit;
    auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(128.0f + v * 127.0f, 0.0f, 255.0f)); };
    c.stick_x = byte(value(Pad::StickRight) - value(Pad::StickLeft));
    c.stick_y = byte(value(Pad::StickDown) - value(Pad::StickUp));
    c.left = static_cast<uint8_t>(value(Pad::LeftTrigger) * 255.0f);
    c.right = static_cast<uint8_t>(value(Pad::RightTrigger) * 255.0f);
    return c;
}

}  // namespace render
