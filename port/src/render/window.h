// The native window: SDL3 for the window, the keyboard and gamepads, and the Metal renderer for
// the picture. It decodes each display list the game hands the PowerVR into a typed frame (draw.h),
// draws it, and at each vertical blank shows the newest render, which the console would scan out
// at the next one (gpu.h).
//
// Keys: Escape, F1 or a gamepad's back button opens the menu (menu.h), F10 shows the frame rate,
// F11 writes the last display list with video memory and the registers, as IKARUGA_DUMP_TA does,
// and F12 writes a screenshot. The menu's QUIT and the window's close button end the run.
#pragma once

#include <chrono>
#include <functional>
#include <cstdint>
#include <map>
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

// Asks for the disc image in the system's open dialog. False when the player cancels.
bool ask_for_disc(std::string& path);
// Shows an error in a message box, for a run that no terminal shows.
void show_error(const std::string& message);

class Window {
public:
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    // A scripted run (autoplay, a replay, scripted presses, a frame limit or screenshots) does not
    // read or write graphics.txt and tweaks.txt, and ignores the MOUSE binding, so that the menu's
    // settings do not change a check.
    // A `scale` other than 0 replaces the GRAPHICS page's.
    bool open(unsigned scale, const std::string& title, bool scripted, bool gamepads, std::string& error);
    // Command-line choices that replace the GRAPHICS page's for this run, before open(): start in
    // fullscreen, and the TATE setting (Graphics::Tate, or -1 to leave it). They are not saved
    // unless the menu then changes the page.
    void start_fullscreen(bool on) { start_fullscreen_ = on; }
    void start_tate(int tate) { start_tate_ = tate; }
    // True while the game is on its VERTICAL SCREEN option, which draws everything sideways for a
    // television turned a quarter. Set by main.cpp, which reads the game's memory. The window turns
    // the picture back for a monitor stood on its side (Graphics::tate).
    std::function<bool()> vertical_screen;
    void render(const std::vector<uint32_t>& stream, const Hardware& hw);
    // Called with each frame between its translation and its drawing (draw.h), where a mod can read
    // and change the frame's draws and settings.
    std::function<void(Frame&)> on_frame;
    // Reads the events and shows the newest render (gpu.h). `guest_seconds` is the console's
    // time, for the frame counter. False once the window is closed.
    bool present(const Hardware& hw, double guest_seconds);
    // The game stops while the menu is open.
    bool menu_open() const { return menu_.is_open(); }
    // Pacing by the display: when a frame of the guest takes close to a whole number of the
    // display's refreshes (2 at 120 Hz, 1 at 60 Hz), each frame stays on the screen for that many.
    // pace() sleeps until shortly before the refresh of the next frame, which the next present asks
    // for. False when the display cannot pace the run (another rate, the window hidden or the menu
    // open, no picture shown yet) or the run is `behind` the guest's clock after a stall. The run
    // then paces by the guest's clock. `frame_seconds` is the guest's last frame.
    bool pace(double frame_seconds, bool behind);
    // The guest's seconds for each second of the host while the display paces the run, else 1.
    double speed() const { return paced_ ? pace_speed_ : 1.0; }
    // The mods and tweaks that the game loop applies (game_tweaks.h).
    const Tweaks& tweaks() const { return tweaks_; }
    // The keyboard and the gamepads as they are now. present() reads them too. With IKARUGA_LATENCY,
    // the window shows white instead of the game while a key or a button is held, from the frame
    // that reads it on, and the renderer reports how long each press took from its event to the
    // screen, for a camera and without one.
    void read_input();
    Controls controls(unsigned player) const { return input_.controls(player); }
    // With the MOUSE binding on, player 1 on the keyboard and the menu closed: the mouse's movement
    // since the last call, in the game's 640x480 picture, and its buttons as player 1's bits (left
    // A, right B, middle X). The window then holds the pointer in SDL's relative mode, so macOS
    // neither moves nor shows it. False otherwise.
    bool mouse(float& dx, float& dy, uint16_t& buttons);
    // The scanned-out framebuffer with the bloom as a PPM file, at the size the geometry is drawn at.
    bool screenshot(const Hardware& hw, const std::string& path);

    Gpu& gpu() { return *gpu_; }

private:
    void key_down(int scancode, const Hardware& hw);
    void pad_button(int button);
    void menu_key(Menu::Key k);
    void menu_done(Menu::Result r);  // closes the menu, or quits, as a key or a click asked
    void open_menu();
    void close_menu();
    void follow_fullscreen(bool on);
    void capture(const Hardware& hw);
    void draw_overlay();

    std::unique_ptr<Gpu> gpu_ = std::make_unique<Gpu>();  // released before the window
    SDL_Window* window_ = nullptr;
    TaDecoder decoder_;
    Input input_;
    Menu menu_;
    Overlay overlay_;
    Graphics graphics_;
    Tweaks tweaks_;
    std::string bindings_path_, graphics_path_, tweaks_path_;
    std::vector<uint32_t> last_stream_;  // for F11
    std::map<uint32_t, Frame> frames_;   // the last frame drawn into each framebuffer
    uint32_t newest_ = 0;                // the framebuffer of the last frame
    bool closed_ = false, show_rate_ = false, scripted_ = false, gamepads_ = true;
    bool fullscreen_ = false;  // asked for, or reported by the window
    bool start_fullscreen_ = false;
    int start_tate_ = -1;
    unsigned rotation_ = 0;    // how the last present turned the picture (Gpu::set_rotation)
    void choose_rotation();
    bool relative_ = false;    // the mouse steers (mouse())
    // Pacing (pace()): the refresh that the next present asks for, how long before it a frame
    // starts, when the last frame started, how far late frames have put the run behind, whether the
    // next frame goes without a present to make up for them, and the recent frames' time from their
    // start to the end of their render on the GPU.
    bool paced_ = false, immediate_ = false;  // immediate_: VSYNC off in fullscreen
    double refresh_ = 0, lead_ = 0.02, pace_speed_ = 1, woke_ = 0, owed_ = 0;
    bool skip_ = false;
    uint64_t skipped_ = 0;
    std::vector<double> works_;
    size_t work_next_ = 0;
    // IKARUGA_LATENCY (read_input()): the clock of SDL's events on Gpu::now()'s, the last key or
    // button event not yet paired with a frame, whether the last frame read one held, and the event
    // time that the next present marks.
    bool latency_ = false;
    double event_clock_ = 0, unread_press_ = 0, press_mark_ = 0;
    bool held_ = false;
    unsigned screenshots_ = 0, captures_ = 0;
    // The frame counter: frames shown per wall-clock second, and guest time per wall-clock time.
    std::chrono::steady_clock::time_point rate_mark_{};
    uint64_t presents_ = 0, presents_at_mark_ = 0;
    double guest_at_mark_ = 0, rate_ = 0, speed_ = 0;
    std::chrono::steady_clock::time_point shown_{};  // the last picture sent to the screen
};

}  // namespace render
