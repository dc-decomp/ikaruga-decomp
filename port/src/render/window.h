// The native window: SDL3 for the window, the keyboard and gamepads, and the Metal renderer for
// the picture. It decodes each display list the game hands the PowerVR, draws it, and at each
// vertical blank shows the framebuffer the console would scan out.
//
// Keys: F1 (or a gamepad's back button) opens the controller screen, F10 shows the frame rate, F11
// writes the last display list with video memory and the registers, as IKARUGA_DUMP_TA does, F12
// writes a screenshot, and Escape quits.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/controls.h"
#include "render/gpu.h"
#include "render/menu.h"
#include "render/overlay.h"
#include "render/ta.h"

struct SDL_Window;

namespace render {

class Window {
public:
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    bool open(unsigned scale, const std::string& title, std::string& error);
    void render(const std::vector<uint32_t>& stream, const Hardware& hw);
    // Reads the events and shows the scanned-out framebuffer. `guest_seconds` is the console's
    // time, for the frame counter. False once the window is closed.
    bool present(const Hardware& hw, double guest_seconds);
    // The game stops while the controller screen is open.
    bool menu_open() const { return menu_.is_open(); }
    Controls controls() const { return input_.controls(); }
    // The scanned-out framebuffer as a PPM file, at the size the geometry is drawn at.
    bool screenshot(const Hardware& hw, const std::string& path);

    Gpu& gpu() { return *gpu_; }

private:
    void key_down(int scancode, const Hardware& hw);
    void pad_button(int button);
    void close_menu();
    void capture(const Hardware& hw);
    void draw_overlay();

    std::unique_ptr<Gpu> gpu_ = std::make_unique<Gpu>();  // released before the window
    SDL_Window* window_ = nullptr;
    TaDecoder decoder_;
    Input input_;
    Menu menu_;
    Overlay overlay_;
    std::string bindings_path_;
    std::vector<uint32_t> last_stream_;  // for F11
    bool closed_ = false, show_rate_ = false;
    unsigned screenshots_ = 0, captures_ = 0;
    // The frame counter: frames shown per wall-clock second, and guest time per wall-clock time.
    std::chrono::steady_clock::time_point rate_mark_{};
    uint64_t presents_ = 0, presents_at_mark_ = 0;
    double guest_at_mark_ = 0, rate_ = 0, speed_ = 0;
    std::chrono::steady_clock::time_point shown_{};  // the last picture sent to the screen
};

}  // namespace render
