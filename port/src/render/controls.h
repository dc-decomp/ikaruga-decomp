// The player's controls: which key, gamepad button or gamepad axis drives each part of the
// Dreamcast controller, and the controller state they give. The bindings live in a text file in
// SDL's preference folder, in the format dream-recomp's launcher writes, so its file can seed ours:
//
//   deadzone = 20            a stick's dead zone in percent
//   trigger_ramp = 1         a key or button on a trigger rises to full over 0.15 s
//   [keyboard]
//   a = key:Z                SDL's scancode names
//   [gamepad]
//   a = button:a             SDL's gamepad button names
//   stick_left = axis:leftx- SDL's gamepad axis names, with the half of the axis that counts
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Gamepad;

namespace render {

enum class Pad : unsigned {
    A, B, X, Y, Start, Up, Down, Left, Right, LeftTrigger, RightTrigger, StickUp, StickDown, StickLeft, StickRight,
    Count
};
constexpr unsigned kPadControls = static_cast<unsigned>(Pad::Count);

// The file's name for a control, and the screen's.
const char* pad_key(Pad c);
const char* pad_label(Pad c);

struct Binding {
    enum class Source { None, Key, Button, Axis } source = Source::None;
    int code = 0;  // an SDL_Scancode, SDL_GamepadButton or SDL_GamepadAxis
    int sign = 1;  // an axis: the half that counts
    std::string text() const;               // "key:Z", "button:a", "axis:leftx-" or "none"
    static Binding parse(const std::string& s);  // Source::None for anything else
};

struct Bindings {
    std::array<Binding, kPadControls> keyboard{}, gamepad{};
    int deadzone_percent = 20;
    bool trigger_ramp = true;
    static Bindings defaults();
    // Unknown lines are reported in `warnings` and skipped: the defaults fill what a file leaves.
    static Bindings parse(const std::string& text, std::string& warnings);
    std::string text() const;
};

// The bindings file, SDL's preference folder for this port, and dream-recomp's launcher's, which
// seeds it when ours does not exist yet.
std::string bindings_path();
std::string dream_recomp_bindings_path();
Bindings load_bindings(const std::string& path, std::string& warnings);
bool save_bindings(const Bindings& b, const std::string& path);

// A Dreamcast controller as the game reads it. The buttons are set while held, in the controller's
// own bits; the stick and triggers are 0 to 255, the stick centred at 128.
struct Controls {
    uint16_t held = 0;
    uint8_t stick_x = 128, stick_y = 128, left = 0, right = 0;
};
constexpr uint16_t kButtonC = 1 << 0, kButtonB = 1 << 1, kButtonA = 1 << 2, kButtonStart = 1 << 3,
                   kButtonUp = 1 << 4, kButtonDown = 1 << 5, kButtonLeft = 1 << 6, kButtonRight = 1 << 7,
                   kButtonY = 1 << 9, kButtonX = 1 << 10;

// Reads the keyboard and every open gamepad through the bindings.
class Input {
public:
    Bindings bindings;
    std::vector<SDL_Gamepad*> pads;
    // Once per frame. `enabled` false gives a released controller, as while a binding is captured.
    void update(bool enabled);
    Controls controls() const;
    float value(Pad c) const { return value_[static_cast<unsigned>(c)]; }

private:
    std::array<float, kPadControls> value_{}, ramp_{};
    uint64_t last_ns_ = 0;
};

}  // namespace render
