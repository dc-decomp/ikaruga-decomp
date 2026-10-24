#include "render/window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace render {

Window::~Window() {
    for (SDL_Gamepad* p : input_.pads) SDL_CloseGamepad(p);
    gpu_.reset();
    if (window_)
        SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

bool Window::open(unsigned scale, const std::string& title, std::string& error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        error = std::string("SDL: ") + SDL_GetError();
        return false;
    }
    window_ = SDL_CreateWindow(title.c_str(), 960, 720,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL);
    if (!window_) {
        error = std::string("no window: ") + SDL_GetError();
        return false;
    }
    if (!gpu_->create(window_, scale, error))
        return false;
    std::printf("window: Metal on %s, drawing at %ux%u\n", gpu_->driver(), 640 * scale, 480 * scale);
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
    std::printf("F1 (or a gamepad's back button) opens the controller screen\n");
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
}

void Window::close_menu() {
    if (!menu_.changed())
        return;
    input_.bindings = menu_.bindings();
    if (!bindings_path_.empty() && save_bindings(input_.bindings, bindings_path_))
        std::printf("bindings saved to %s\n", bindings_path_.c_str());
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
        if (menu_.key(k))
            close_menu();
        return;
    }
    switch (s) {
        case SDL_SCANCODE_ESCAPE: closed_ = true; break;
        case SDL_SCANCODE_F1: menu_.open(input_.bindings); break;
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
            menu_.open(input_.bindings);
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
    if (menu_.key(k))
        close_menu();
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
                if (!e.key.repeat || menu_.is_open())
                    key_down(e.key.scancode, hw);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                pad_button(e.gbutton.button);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                // A stick or trigger pushed well past the middle binds its half.
                if (menu_.capturing() && !menu_.for_keyboard() && std::abs(e.gaxis.value) > 24000)
                    menu_.capture(Binding{Binding::Source::Axis, e.gaxis.axis, e.gaxis.value < 0 ? -1 : 1});
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* p = SDL_OpenGamepad(e.gdevice.which))
                    input_.pads.push_back(p);
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
    if (now - shown_ < std::chrono::milliseconds(10))
        return true;
    shown_ = now;
    draw_overlay();
    gpu_->present(hw);
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
    if (!gpu_->read_displayed(hw, pixels, w, h))
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
