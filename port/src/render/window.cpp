#include "render/window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace render {

bool ask_for_disc(std::string& path) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        return false;
    struct Answer {
        bool done = false;
        std::string path;
    } answer;
    static const SDL_DialogFileFilter kFilters[] = {{"Dreamcast disc image (GDI or CHD)", "gdi;chd"}};
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(kFilters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Choose the Ikaruga disc (.gdi or .chd)");
    SDL_ShowFileDialogWithProperties(
        SDL_FILEDIALOG_OPENFILE,
        [](void* data, const char* const* files, int) {
            auto* a = static_cast<Answer*>(data);
            if (files && files[0])
                a->path = files[0];
            a->done = true;
        },
        &answer, props);
    while (!answer.done) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    SDL_DestroyProperties(props);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    path = answer.path;
    return !path.empty();
}

void show_error(const std::string& message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ikaruga", message.c_str(), nullptr);
}

Window::~Window() {
    if (std::getenv("IKARUGA_PACING") && refresh_ > 0)
        std::fprintf(stderr,
                     "pacing: the frames started %.1f ms before their refresh, %llu pictures came late, %llu "
                     "frames went without a picture to make up for them\n",
                     lead_ * 1000, static_cast<unsigned long long>(gpu_->late()),
                     static_cast<unsigned long long>(skipped_));
    for (SDL_Gamepad* p : input_.pads) SDL_CloseGamepad(p);
    gpu_.reset();
    if (window_)
        SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

bool Window::open(unsigned scale, const std::string& title, bool scripted, bool gamepads, std::string& error) {
    scripted_ = scripted;
    gamepads_ = gamepads;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | (gamepads ? SDL_INIT_GAMEPAD : 0))) {
        error = std::string("SDL: ") + SDL_GetError();
        return false;
    }
    window_ = SDL_CreateWindow(title.c_str(), 960, 720,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
#ifdef __APPLE__
                                   SDL_WINDOW_METAL
#else
                                   SDL_WINDOW_VULKAN
#endif
    );
    if (!window_) {
        error = std::string("no window: ") + SDL_GetError();
        return false;
    }
    if (scripted)
        std::printf("settings: a scripted run does not read or write %s or %s\n", settings_path("graphics.txt").c_str(),
                    settings_path("tweaks.txt").c_str());
    else {
        graphics_path_ = settings_path("graphics.txt");
        tweaks_path_ = settings_path("tweaks.txt");
    }
    if (!graphics_path_.empty())
        graphics_ = Graphics::parse(read_settings(graphics_path_));
    if (!tweaks_path_.empty())
        tweaks_ = Tweaks::parse(read_settings(tweaks_path_));
    if (scale)
        graphics_.scale = scale;
    if (!gpu_->create(window_, graphics_.scale, error))
        return false;
    gpu_->set_anisotropy(graphics_.anisotropy);
    gpu_->set_hud(graphics_.hud);
    std::printf("window: %s, drawing at %ux%u\n", gpu_->driver(), 640 * graphics_.scale, 480 * graphics_.scale);
    // The bindings: ours, or on a first run dream-recomp's launcher's, or the defaults.
    bindings_path_ = bindings_path();
    std::string warnings;
    std::error_code ec;
    const bool ours = !bindings_path_.empty() && std::filesystem::exists(bindings_path_, ec);
    const std::string from = ours ? bindings_path_ : dream_recomp_bindings_path();
    input_.bindings = load_bindings(from, warnings);
    if (!warnings.empty())
        std::fprintf(stderr, "%s:\n%s", from.c_str(), warnings.c_str());
    if (!bindings_path_.empty())
        std::printf("bindings: %s%s\n", bindings_path_.c_str(), ours ? "" : " (not written yet)");
    std::printf("Escape, F1 or a gamepad's back button opens the menu\n");
    SDL_HideCursor();  // the game has no use for it, and the menu shows it
    latency_ = std::getenv("IKARUGA_LATENCY") != nullptr;
    event_clock_ = Gpu::now() - static_cast<double>(SDL_GetTicksNS()) * 1e-9;
    if (latency_)
        std::printf("latency test: the window shows white while a key or a button is held\n");
    return true;
}

void Window::render(const std::vector<uint32_t>& stream, const Hardware& hw) {
    last_stream_ = stream;
    decoder_.reset();
    decoder_.feed(stream.data(), stream.size());
    Frame frame = translate(decoder_.list(), hw);
    if (on_frame)
        on_frame(frame);
    gpu_->render(frame, hw);
    const uint32_t target = frame.target;
    newest_ = target;
    frames_[target] = std::move(frame);
}

void Window::open_menu() {
    if (relative_) {
        SDL_SetWindowRelativeMouseMode(window_, false);
        relative_ = false;
    }
    menu_.open(input_.bindings, graphics_, tweaks_);
    SDL_ShowCursor();
}

void Window::close_menu() {
    SDL_HideCursor();
    if (menu_.bindings_changed()) {
        input_.bindings = menu_.bindings();
        if (!bindings_path_.empty() && save_bindings(input_.bindings, bindings_path_))
            std::printf("bindings saved to %s\n", bindings_path_.c_str());
    }
    if (menu_.graphics_changed()) {
        graphics_ = menu_.graphics();
        if (!graphics_path_.empty() && write_settings(graphics_path_, graphics_.text()))
            std::printf("graphics saved to %s\n", graphics_path_.c_str());
    }
    if (menu_.tweaks_changed()) {
        tweaks_ = menu_.tweaks();
        if (!tweaks_path_.empty() && write_settings(tweaks_path_, tweaks_.text()))
            std::printf("tweaks saved to %s\n", tweaks_path_.c_str());
    }
}

// The window's fullscreen state, which its green button and Control-Command-F change too, goes to
// the settings and to the menu's copy.
void Window::follow_fullscreen(bool on) {
    fullscreen_ = on;
    menu_.set_fullscreen(on);
    if (graphics_.fullscreen == on)
        return;
    graphics_.fullscreen = on;
    if (!graphics_path_.empty() && write_settings(graphics_path_, graphics_.text()))
        std::printf("graphics saved to %s\n", graphics_path_.c_str());
}

void Window::menu_key(Menu::Key k) { menu_done(menu_.key(k)); }

void Window::menu_done(Menu::Result r) {
    if (r == Menu::Result::Stay)
        return;
    close_menu();
    if (r == Menu::Result::Quit)
        closed_ = true;
}

void Window::key_down(int scancode, const Hardware& hw) {
    const auto s = static_cast<SDL_Scancode>(scancode);
    if (menu_.is_open()) {
        if (menu_.capturing() && menu_.for_keyboard() && s != SDL_SCANCODE_ESCAPE) {
            menu_.capture(Binding{Binding::Source::Key, scancode, 1});
            return;
        }
        Menu::Key k;
        switch (s) {
            case SDL_SCANCODE_UP: k = Menu::Key::Up; break;
            case SDL_SCANCODE_DOWN: k = Menu::Key::Down; break;
            case SDL_SCANCODE_LEFT: k = Menu::Key::Left; break;
            case SDL_SCANCODE_RIGHT: k = Menu::Key::Right; break;
            case SDL_SCANCODE_RETURN: k = Menu::Key::Accept; break;
            case SDL_SCANCODE_BACKSPACE:
            case SDL_SCANCODE_DELETE: k = Menu::Key::Clear; break;
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_F1: k = Menu::Key::Back; break;
            default: return;
        }
        menu_key(k);
        return;
    }
    switch (s) {
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_F1: open_menu(); break;
        case SDL_SCANCODE_F10: show_rate_ = !show_rate_; break;
        case SDL_SCANCODE_F11: capture(hw); break;
        case SDL_SCANCODE_F12: {
            char name[32];
            std::snprintf(name, sizeof name, "screenshot-%03u.ppm", screenshots_++);
            if (screenshot(hw, name))
                std::printf("wrote %s\n", name);
            break;
        }
        default: break;
    }
}

void Window::pad_button(int button) {
    const auto b = static_cast<SDL_GamepadButton>(button);
    if (!menu_.is_open()) {
        if (b == SDL_GAMEPAD_BUTTON_BACK)
            open_menu();
        return;
    }
    if (menu_.capturing() && !menu_.for_keyboard()) {
        menu_.capture(Binding{Binding::Source::Button, button, 1});
        return;
    }
    Menu::Key k;
    switch (b) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: k = Menu::Key::Up; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: k = Menu::Key::Down; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: k = Menu::Key::Left; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: k = Menu::Key::Right; break;
        case SDL_GAMEPAD_BUTTON_SOUTH: k = Menu::Key::Accept; break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_BACK: k = Menu::Key::Back; break;
        default: return;
    }
    menu_key(k);
}

bool Window::present(const Hardware& hw, double guest_seconds) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                closed_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (latency_ && !e.key.repeat)
                    unread_press_ = event_clock_ + static_cast<double>(e.key.timestamp) * 1e-9;
                if (!e.key.repeat || menu_.is_open())
                    key_down(e.key.scancode, hw);
                break;
            // The menu takes the mouse in the overlay's pixels.
            case SDL_EVENT_MOUSE_MOTION:
                if (menu_.is_open()) {
                    const float d = SDL_GetWindowPixelDensity(window_);
                    menu_.point(e.motion.x * d, e.motion.y * d);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (menu_.is_open() && (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)) {
                    const float d = SDL_GetWindowPixelDensity(window_);
                    menu_done(menu_.click(e.button.x * d, e.button.y * d, e.button.button == SDL_BUTTON_RIGHT));
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (menu_.is_open() && e.wheel.y != 0)
                    menu_key(e.wheel.y > 0 ? Menu::Key::Up : Menu::Key::Down);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (latency_)
                    unread_press_ = event_clock_ + static_cast<double>(e.gbutton.timestamp) * 1e-9;
                pad_button(e.gbutton.button);
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                follow_fullscreen(e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                // A stick or trigger pushed well past the middle binds its half.
                if (menu_.capturing() && !menu_.for_keyboard() && std::abs(e.gaxis.value) > 24000)
                    menu_.capture(Binding{Binding::Source::Axis, e.gaxis.axis, e.gaxis.value < 0 ? -1 : 1});
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (gamepads_)
                    if (SDL_Gamepad* p = SDL_OpenGamepad(e.gdevice.which)) {
                        std::fprintf(stderr, "input: opened gamepad %s\n", SDL_GetGamepadName(p));
                        input_.pads.push_back(p);
                    }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (auto it = input_.pads.begin(); it != input_.pads.end(); ++it)
                    if (SDL_GetGamepadID(*it) == e.gdevice.which) {
                        SDL_CloseGamepad(*it);
                        input_.pads.erase(it);
                        break;
                    }
                break;
            default:
                break;
        }
    }
    if (closed_)
        return false;
    input_.update(!menu_.is_open());
    ++presents_;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - rate_mark_).count();
    if (rate_mark_ == std::chrono::steady_clock::time_point{} || elapsed >= 0.25) {
        if (rate_mark_ != std::chrono::steady_clock::time_point{}) {
            rate_ = static_cast<double>(presents_ - presents_at_mark_) / elapsed;
            speed_ = (guest_seconds - guest_at_mark_) / elapsed;
        }
        rate_mark_ = now;
        presents_at_mark_ = presents_;
        guest_at_mark_ = guest_seconds;
    }
    // The picture goes to the screen at most every 10 ms, and not while the window cannot be seen:
    // macOS takes a background window's pictures at the display's rate, and waiting for it to take
    // one would hold the whole run to that rate.
    if (SDL_GetWindowFlags(window_) & (SDL_WINDOW_OCCLUDED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
        return true;
    const bool paced = paced_ && !menu_.is_open();
    if (!paced && now - shown_ < std::chrono::milliseconds(10))
        return true;
    if (paced && skip_)
        return true;
    shown_ = now;
    const Graphics& g = menu_.is_open() ? menu_.graphics() : graphics_;
    if (g.scale != gpu_->scale() || g.anisotropy != gpu_->anisotropy()) {
        gpu_->set_scale(g.scale);
        gpu_->set_anisotropy(g.anisotropy);
        // The frames on the framebuffers again, the newest last, which the window shows, so that the
        // picture behind the menu shows the change. Their textures come from video memory as it is now.
        for (const auto& [target, frame] : frames_)
            if (target != newest_)
                gpu_->render(frame, hw);
        if (auto it = frames_.find(newest_); it != frames_.end())
            gpu_->render(it->second, hw);
    }
    gpu_->set_bloom(g.bloom);
    gpu_->set_hud(g.hud);
    if (g.fullscreen != fullscreen_) {
        fullscreen_ = g.fullscreen;
        SDL_SetWindowFullscreen(window_, fullscreen_);
    }
    draw_overlay();
    const double ready = Gpu::now();
    // Shown at once, a picture goes out at the same time after each frame's start, so that the GPU's
    // time does not shake the cadence: 3.5 ms before the refresh, or when the GPU has drawn it.
    if (paced && immediate_) {
        // One sleep to shortly before the time, then short ones: each waking costs system calls.
        const double until = std::min(refresh_ - 0.0035, ready + 0.02);
        if (until - Gpu::now() > 0.001)
            std::this_thread::sleep_for(std::chrono::duration<double>(until - Gpu::now() - 0.0006));
        while (Gpu::now() < until) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    gpu_->present(hw, paced && !immediate_ ? refresh_ : 0, latency_ && held_, press_mark_);
    press_mark_ = 0;
    if (const double work = gpu_->take_work(); paced && work > 0) {
        if (works_.size() < 240)
            works_.push_back(work);
        else
            works_[work_next_++ % 240] = work;
    }
    return true;
}

bool Window::mouse(float& dx, float& dy, uint16_t& buttons) {
    const bool steer = input_.bindings.mouse && input_.on_keyboard(0) && !menu_.is_open() && !scripted_;
    if (steer != relative_) {
        SDL_SetWindowRelativeMouseMode(window_, steer);
        relative_ = steer;
        SDL_GetRelativeMouseState(nullptr, nullptr);  // the movement from here on
    }
    if (!steer)
        return false;
    float rx = 0, ry = 0;
    SDL_GetRelativeMouseState(&rx, &ry);
    const SDL_MouseButtonFlags held = SDL_GetMouseState(nullptr, nullptr);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return false;
    // The 4:3 picture in the middle of the window, as Gpu::present() places it, so the ship moves as
    // far as the pointer would.
    const float d = SDL_GetWindowPixelDensity(window_);
    float w = static_cast<float>(pw), h = w * 3 / 4;
    if (h > static_cast<float>(ph)) {
        h = static_cast<float>(ph);
        w = h * 4 / 3;
    }
    dx = rx * d / w * 640;
    dy = ry * d / h * 480;
    buttons = static_cast<uint16_t>((held & SDL_BUTTON_LMASK ? 1u << 2 : 0) | (held & SDL_BUTTON_RMASK ? 1u << 1 : 0) |
                                    (held & SDL_BUTTON_MMASK ? 1u << 10 : 0));
    return true;
}

void Window::read_input() {
    SDL_PumpEvents();
    input_.update(!menu_.is_open());
    if (!latency_)
        return;
    // The presses that wait in the queue, which present() takes later.
    SDL_Event waiting[16];
    const int n = SDL_PeepEvents(waiting, 16, SDL_PEEKEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP);
    for (int i = 0; i < n; ++i) {
        const SDL_Event& e = waiting[i];
        if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
            unread_press_ = std::max(unread_press_, event_clock_ + static_cast<double>(e.common.timestamp) * 1e-9);
    }
    int keys = 0;
    const bool* state = SDL_GetKeyboardState(&keys);
    bool held = false;
    for (int k = 0; k < keys && !held; ++k) held = state[k];
    for (SDL_Gamepad* p : input_.pads)
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !held; ++b)
            held = SDL_GetGamepadButton(p, static_cast<SDL_GamepadButton>(b));
    if (held && !held_ && unread_press_ > 0)
        press_mark_ = unread_press_;
    if (!held || press_mark_ > 0)
        unread_press_ = 0;
    held_ = held;
}

bool Window::pace(double frame_seconds, bool behind) {
    const double period = gpu_->refresh_period();
    const double refreshes = frame_seconds / period;
    const long n = std::lround(refreshes);
    const bool hidden =
        SDL_GetWindowFlags(window_) & (SDL_WINDOW_OCCLUDED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN);
    // A frame of more than four refreshes is a stall or a pause, which the display does not pace.
    if (n < 1 || n > 4 || std::abs(refreshes / static_cast<double>(n) - 1) > 0.005 || hidden || menu_.is_open() ||
        behind) {
        paced_ = false;
        refresh_ = 0;
        woke_ = 0;
        owed_ = 0;
        skip_ = false;
        // Paced by the guest's clock, the presents wait for the refresh with VSYNC on, but not while
        // the run catches up, which they would hold back.
        gpu_->set_display_sync(graphics_.vsync && !behind);
        return false;
    }
    // VSYNC off in fullscreen: the window server then shows each picture at once, about 2 ms after
    // its present instead of two refreshes, and a picture can change in the middle of a refresh. In
    // a window the window server waits for the refresh anyway.
    const bool immediate = !graphics_.vsync && fullscreen_;
    immediate_ = immediate;
    gpu_->set_display_sync(!immediate);
    const double shown = gpu_->presented();
    if (shown <= 0)
        return false;
    // A picture takes two to two and a half refreshes from its present to the screen, through Core
    // Animation and the window server, and a frame takes its work from its start to the end of its
    // render on the GPU: the 99th percentile of the last 240 frames, so that one slow frame does not
    // move the start. A larger lead would only queue the pictures, which then come late.
    const double frame = static_cast<double>(n) * period;
    double work = 0.005;
    if (works_.size() >= 60) {
        std::vector<double> w = works_;
        std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() * 99 / 100), w.end());
        work = w[w.size() * 99 / 100];
    }
    // With the pictures shown at once, the lead only places the frames: a frame reads the controls
    // its work and 3.5 ms before its time, and starts after the last one's picture went out.
    lead_ = immediate ? std::clamp(0.0035 + work, 0.004, frame + 0.003)
                      : std::clamp(2.5 * period + 0.002 + work, 0.004, 0.05);
    paced_ = true;
    pace_speed_ = frame_seconds / frame;
    // A whole frame owed: this frame goes without a picture, and the run goes on at once.
    skip_ = owed_ >= frame - period / 2;
    if (skip_) {
        owed_ -= frame;
        ++skipped_;
        woke_ = 0;
        return true;
    }
    // The refresh n after the last one asked for, on the grid of the refreshes that showed pictures.
    const double now = Gpu::now();
    // Shown at once, the pictures come off the grid, so the frames keep their own cadence.
    double next = refresh_ > 0 ? refresh_ + frame : shown + frame;
    // With direct scanout, a picture shows when its time comes, half a refresh off the grid, and
    // snapping to it would pull each frame forward.
    if (const double k = (next - shown) / period; !immediate && std::abs(k - std::round(k)) < 0.25)
        next = shown + std::round(k) * period;
    // A frame that cannot make its refresh takes the next one that it can make, and the run owes the
    // guest's clock the time, which the sound's queue would lose. Later frames make it up: a frame
    // comes refreshes earlier while it still comes after the last one, and a whole frame owed skips a
    // picture. More than the sound's queue holds is lost already, so the run owes at most that.
    const long back = std::min({std::lround(std::floor(owed_ / period + 0.01)), n - 1,
                                std::lround(std::floor((next - lead_ - now) / period))});
    if (back > 0) {
        next -= static_cast<double>(back) * period;
        owed_ -= static_cast<double>(back) * period;
    }
    if (next - lead_ < now) {
        const double later = std::ceil((now + lead_ - next) / period) * period;
        next += later;
        owed_ = std::min(owed_ + later, 3 * frame);
    }
    refresh_ = next;
    if (next - lead_ > now)
        std::this_thread::sleep_for(std::chrono::duration<double>(next - lead_ - now));
    woke_ = Gpu::now();
    gpu_->start_frame(woke_);
    return true;
}

void Window::draw_overlay() {
    if (!menu_.is_open() && !show_rate_) {
        if (!overlay_.empty()) {
            overlay_.clear();
            gpu_->set_overlay(nullptr, 0, 0);
        }
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    overlay_.begin(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    if (show_rate_) {
        char line[64];
        std::snprintf(line, sizeof line, "%.1f FPS  %.2fX", rate_, speed_);
        const float size = std::max(12.0f, static_cast<float>(h) / 40.0f);
        overlay_.box(size * 0.4f, size * 0.4f, overlay_.text_width(line, size) + size * 0.8f, size * 1.6f, 0xA0000000u);
        overlay_.text(size * 0.8f, size * 0.7f, line, size, 0xFFFFFFFFu);
    }
    if (menu_.is_open())
        menu_.draw(overlay_, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    gpu_->set_overlay(overlay_.pixels().data(), overlay_.width(), overlay_.height());
}

// The last display list, video memory and the registers, named as IKARUGA_DUMP_TA names them, so
// render-view draws them again.
void Window::capture(const Hardware& hw) {
    char stem[32];
    std::snprintf(stem, sizeof stem, "capture-%03u.ta", captures_++);
    auto write = [](const std::string& path, const void* data, size_t bytes) {
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(data, 1, bytes, f);
            std::fclose(f);
        }
    };
    write(stem, last_stream_.data(), last_stream_.size() * 4);
    write(std::string(stem) + ".vram", hw.vram, hw.vram_size);
    write(std::string(stem) + ".vram.regs", hw.registers, 0x2000);
    std::printf("wrote %s with its .vram and .vram.regs\n", stem);
}

bool Window::screenshot(const Hardware& hw, const std::string& path) {
    std::vector<uint32_t> pixels;
    uint32_t w = 0, h = 0;
    if (!gpu_->read_displayed(hw, pixels, w, h, true))
        return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t p : pixels) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8),
                                      static_cast<unsigned char>(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

}  // namespace render
