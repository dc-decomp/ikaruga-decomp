// The F1 screen: the controller bindings for the keyboard and for gamepads, the dead zone and the
// trigger ramp. The game stops while it is open. It works on a copy of the bindings, which the
// window takes back, and saves, when the screen closes.
#pragma once

#include "render/controls.h"
#include "render/overlay.h"

namespace render {

class Menu {
public:
    enum class Key { Up, Down, Left, Right, Accept, Back, Clear };

    bool is_open() const { return open_; }
    void open(const Bindings& b);
    // True when the screen closed on this key.
    bool key(Key k);
    // Capturing an input for the chosen row: the window gives it the next key or gamepad input of
    // the chosen device, or cancels with Back.
    bool capturing() const { return capturing_; }
    bool for_keyboard() const { return keyboard_; }
    void capture(const Binding& b);
    const Bindings& bindings() const { return bindings_; }
    bool changed() const { return changed_; }
    void draw(Overlay& o, uint32_t width, uint32_t height) const;

private:
    // The rows: the device, the controls, the dead zone, the trigger ramp, and DONE.
    static constexpr int kDevice = 0, kFirstControl = 1, kDeadzone = kFirstControl + kPadControls,
                         kRamp = kDeadzone + 1, kDone = kRamp + 1, kRows = kDone + 1;
    Bindings bindings_;
    int row_ = kFirstControl;
    bool open_ = false, keyboard_ = true, capturing_ = false, changed_ = false;
};

}  // namespace render
