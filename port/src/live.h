// The window: the picture, the controller bindings and the screenshots. This is the launcher's
// Live from dream-recomp (runtime/boot/boot_main.cpp), unchanged but for its namespace: the port
// draws with dream-recomp's renderer, so it shows the console's frame the same way. The guest
// nominates what is shown: at the vertical blank the address in FB_R_SOF1 decides, and a buffer
// the renderer drew into is shown directly, at the resolution --scale asks for.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dream/render/background.h"
#include "dream/render/display_list.h"
#include "dream/render/fog.h"
#include "dream/render/framebuffer.h"
#include "dream/render/input_menu.h"
#include "dream/render/overlay.h"
#include "dream/render/vk/offscreen.h"
#include "dream/render/vk/present.h"
#include "dream/render/vk/renderer.h"
#include "dream/render/vk/window.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/core.h"

namespace port {

struct Live {
    Live(dream::mem::DcMemory& memory, const dream::pvr::Core& pvr) : memory_(memory), pvr_(pvr) {}

    // `scale` multiplies the resolution the geometry is drawn at. The frame is resampled back to
    // the guest's own framebuffer on the way into video memory, so a higher setting sharpens the
    // geometry without lying to the guest about the size of its screen.
    bool start(unsigned scale, bool validation, const std::string& title) {
        const std::uint32_t w = kGuestWidth * scale, h = kGuestHeight * scale;
        if (!window.create(title.c_str(), 960, 720, validation)) {
            error = window.error();
            return false;
        }
        if (!offscreen.create(window.context(), w, h)) {
            error = offscreen.error();
            return false;
        }
        // The renderer's pipelines are built against the offscreen render pass, not the window's:
        // that is where the geometry is drawn.
        if (!renderer.create(window.context(), offscreen.render_pass())) {
            error = renderer.error();
            return false;
        }
        if (!presenter.create(window.context(), window.render_pass())) {
            error = presenter.error();
            return false;
        }
        renderer.set_memory(memory_.vram(), dream::mem::DcMemory::kVramSize,
                            pvr_.reg_block() + 0x1000 / 4, palette_format());
        std::printf("window: %s, drawing at %ux%u\n", window.context().caps().device_name.c_str(),
                    w, h);
        return true;
    }

    // The guest has handed the hardware a display list. Draw it, then put the result where the
    // hardware would have put it.
    void render(const std::vector<std::uint32_t>& stream) {
        last_stream = stream;  // kept so F11 can write the list that drew what is on screen
        decoder.reset();
        decoder.feed_stream(stream.data(), stream.size());
        // The plane the whole frame sits on is not in the stream: the title writes it into the
        // parameter buffer and points a register at it. Without it the screen behind the geometry
        // is whatever the renderer cleared to.
        if (dream::render::add_background(decoder.frame(), pvr_.reg_block(), memory_.vram(),
                                          dream::mem::DcMemory::kVramSize))
            ++backgrounds;
        const dream::render::Frame& frame = decoder.frame();
        // PT_ALPHA_REF: the punch-through threshold the guest chose, which changes per scene.
        geometry.alpha_ref = static_cast<float>(pvr_.reg(0x11C) & 0xFFu) / 255.0f;
        // The fog registers and table, which the title can change at any time.
        dream::render::describe_fog(pvr_.reg_block(), geometry.fog);
        // Palette memory is re-read every render but only invalidates the cache when it has
        // actually changed, which is rare; dropping it every frame would decode every texture
        // every frame.
        if (renderer.textures().set_palette(pvr_.reg_block() + 0x1000 / 4, palette_format()))
            ++palette_changes;
        if (!offscreen.render(renderer, frame, geometry)) {
            if (!reported_error) {
                reported_error = true;
                std::fprintf(stderr, "render: %s\n", offscreen.error().c_str());
            }
            return;
        }
        write_back();
        ++rendered;
    }

    // True for a buffer the renderer has drawn into at some point. A title alternates between two
    // and does not swap the displayed one on every render, so asking whether the displayed buffer
    // is *the* most recent target is too strict; asking whether it is one of ours is the question
    // that matters. A buffer we have never rendered to holds pixels the title put there itself,
    // and those have to come out of video memory.
    bool rendered_into(std::uint32_t address) const {
        for (std::uint32_t a : targets_)
            if (a == address)
                return true;
        return false;
    }

    // Where the frame went, and optionally the frame itself. The address is always worked out,
    // because presentation needs to know which buffer holds what the renderer drew.
    void write_back() {
        dream::render::FramebufferInfo display;
        std::uint32_t w = kGuestWidth, h = kGuestHeight;
        if (dream::render::describe_framebuffer(pvr_.reg_block(), display)) {
            w = display.width;
            h = display.height;
        }
        dream::render::FramebufferInfo target;
        if (!dream::render::describe_write_framebuffer(pvr_.reg_block(), w, h, target))
            return;
        target_address = target.address;
        if (!rendered_into(target.address) && targets_.size() < 8)
            targets_.push_back(target.address);
        if (!writeback)
            return;
        if (!dream::render::encode_framebuffer(target, offscreen.pixels(), offscreen.width(),
                                               offscreen.height(), memory_.vram(),
                                               dream::mem::DcMemory::kVramSize))
            return;
        ++written_back;
        // Whatever the cache decoded from those bytes is now out of date. A title that is merely
        // double buffering has nothing cached there and loses nothing; a title rendering a texture
        // gets the frame it just drew.
        const std::uint32_t bytes =
            (target.width + target.modulus) * target.height * bytes_per_written_pixel(target);
        // DREAM_NO_WRITEBACK_INVALIDATE holds the cache across the write, to tell a texture that
        // went wrong here apart from one that was already wrong: if the picture does not change,
        // the fault is not in this step.
        if (!std::getenv("DREAM_NO_WRITEBACK_INVALIDATE"))
            renderer.textures().invalidate_range(target.address, target.address + bytes);
    }

    // Vertical blank: show what the video hardware would be scanning out, and read the keyboard.
    // Returns false when the user has closed the window.
    bool present(std::uint64_t frame, std::uint64_t guest_cycles) {
        guest_frame = frame;
        if (!window.poll())
            return false;
        update_counter(guest_cycles);
        pump_menu();

        dream::render::FramebufferInfo fb;
        const bool described = dream::render::describe_framebuffer(pvr_.reg_block(), fb);
        // The guest is displaying the buffer the renderer drew into, so show what was drawn rather
        // than a copy of it squeezed back through the guest's pixel format. This is also what makes
        // --scale visible: the window gets the full rendered resolution.
        from_renderer = described && fb.enabled && rendered_into(fb.address);
        if (from_renderer) {
            shown = fb;
            shown.width = offscreen.width();
            shown.height = offscreen.height();
            have_frame = true;
        } else if (described && fb.enabled &&
                   dream::render::decode_framebuffer(fb, memory_.vram(),
                                                     dream::mem::DcMemory::kVramSize, pixels)) {
            shown = fb;
            have_frame = true;
            ++decoded_frames;
        }
        if (!have_frame)
            return true;  // nothing displayable yet: the window keeps its cleared background

        std::uint32_t image = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!window.begin_frame(image, cmd)) {
            window.recreate_swapchain();
            return true;
        }
        const std::uint32_t* source = from_renderer ? offscreen.pixels() : pixels.data();
        // Composited into the frame as it is handed to the GPU, so it costs no extra copy of the
        // picture. Deliberately not into the buffers screenshot() and capture() read: F12 and F11
        // are meant to produce the game's own pixels, which is what makes one comparable with
        // another and usable as a reference image.
        if (show_fps || menu.is_open())
            presenter.upload(cmd, source, shown.width, shown.height,
                             [this](std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
                                 if (show_fps)
                                     draw_counter(px, w, h);
                                 // Last, so the screen sits over the counter rather than under it.
                                 if (menu.is_open())
                                     menu.draw(px, w, w, h, window.devices());
                             });
        else
            presenter.upload(cmd, source, shown.width, shown.height);
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].depthStencil = {0.0f, 0};
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = window.render_pass();
        rpbi.framebuffer = window.framebuffer(image);
        rpbi.renderArea.extent = window.extent();
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(window.extent().width),
                                  static_cast<float>(window.extent().height),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, window.extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        presenter.draw(cmd, window.extent());
        vkCmdEndRenderPass(cmd);
        if (!window.end_frame(image))
            window.recreate_swapchain();
        ++presented;
        if (window.pressed(dream::render::vk::Control::ToggleFps)) {
            show_fps = !show_fps;
            fps_mark = {};  // start the next average fresh rather than from whenever it last ran
        }
        if (window.pressed(dream::render::vk::Control::Screenshot))
            screenshot();
        if (window.pressed(dream::render::vk::Control::Capture))
            capture();
        if (screenshot_at && presented == screenshot_at)
            screenshot();
        if (capture_at && presented == capture_at)
            capture();
        return true;
    }

    // What the player is pressing, as a Maple controller sees it. The buttons are active low.
    //
    // Everything here comes through the player's bindings rather than off fixed keys, so a pad and
    // the keyboard are both live and either can be rebound. The stick and the triggers are read as
    // values rather than as flags: a real pad's travel reaches the guest, and a keyboard still
    // produces the extremes it always did, because a digital source reads a full 1.0.
    void read_controls(dream::maple::ControllerState& pad) const {
        using dream::render::PadControl;
        std::uint16_t held = 0;
        if (window.pad_held(PadControl::Up))
            held |= dream::maple::kUp;
        if (window.pad_held(PadControl::Down))
            held |= dream::maple::kDown;
        if (window.pad_held(PadControl::Left))
            held |= dream::maple::kLeft;
        if (window.pad_held(PadControl::Right))
            held |= dream::maple::kRight;
        if (window.pad_held(PadControl::A))
            held |= dream::maple::kA;
        if (window.pad_held(PadControl::B))
            held |= dream::maple::kB;
        if (window.pad_held(PadControl::X))
            held |= dream::maple::kX;
        if (window.pad_held(PadControl::Y))
            held |= dream::maple::kY;
        if (window.pad_held(PadControl::Start))
            held |= dream::maple::kStart;
        pad.buttons = static_cast<std::uint16_t>(0xFFFFu & ~held);
        pad.joy_x = dream::maple::axis_byte(window.pad_value(PadControl::StickLeft),
                                            window.pad_value(PadControl::StickRight));
        pad.joy_y = dream::maple::axis_byte(window.pad_value(PadControl::StickUp),
                                            window.pad_value(PadControl::StickDown));
        pad.ltrigger = dream::maple::trigger_byte(window.pad_value(PadControl::LeftTrigger));
        pad.rtrigger = dream::maple::trigger_byte(window.pad_value(PadControl::RightTrigger));
    }

    // --- the binding screen --------------------------------------------------------------------

    // Its keys are the launcher's fixed ones, never the player's: rebinding the game's buttons can
    // never take away the way back out. Called once per poll, running or paused.
    void pump_menu() {
        using dream::render::vk::Control;
        // Escape does two things a keystroke apart: leave the screen, then end the run. The key has
        // to be let go before it means the second one. Without this, closing the screen with escape
        // re-arms quit under a finger that is still down, and one key repeat ends the run -- which
        // from the outside is indistinguishable from a crash.
        if (rearm_escape && !window.held(Control::Back)) {
            rearm_escape = false;
            window.set_escape_quits(true);
        }
        if (window.pressed(Control::Menu)) {
            if (menu.is_open())
                close_menu();
            else
                open_menu();
            return;
        }
        if (!menu.is_open())
            return;
        // A finished capture takes the frame to itself. The escape that cancelled one is also a
        // Back press, and closing the screen on it would make cancelling and leaving the same
        // keystroke.
        dream::render::Binding captured;
        bool cancelled = false;
        if (window.take_capture(captured, cancelled)) {
            menu.apply_capture(captured, cancelled);
            apply_bindings();
            return;
        }
        if (window.pressed(Control::Back)) {
            menu.back();  // one leaves a capture, a second leaves the screen
            if (!menu.is_open())
                close_menu();
            return;
        }
        if (window.pressed(Control::Up))
            menu.move(-1);
        if (window.pressed(Control::Down))
            menu.move(1);
        // The device row counts what is actually plugged in, so the selector wraps over the
        // keyboard and however many pads are connected right now.
        const unsigned devices = static_cast<unsigned>(window.devices().size());
        if (window.pressed(Control::Left))
            menu.adjust(-1, devices);
        if (window.pressed(Control::Right))
            menu.adjust(1, devices);
        if (window.pressed(Control::Start) || window.pressed(Control::A)) {
            if (menu.activate() == dream::render::InputMenu::Action::Close) {
                close_menu();
                return;
            }
            // Activating a binding row asks for an input; the window collects it for us.
            if (menu.awaiting())
                window.begin_capture();
        }
        apply_bindings();
    }

    void open_menu() {
        menu.open(window.bindings());
        bindings_dirty = false;
        rearm_escape = false;
        // While the screen is up, escape backs out of it rather than ending the run.
        window.set_escape_quits(false);
    }

    void close_menu() {
        menu.close();
        window.cancel_capture();
        rearm_escape = true;  // not until escape is released; see pump_menu()
        if (!bindings_dirty)
            return;
        bindings_dirty = false;
        if (bindings_path.empty()) {
            std::printf("bindings changed for this run only: no file to save them in\n");
            return;
        }
        if (save_bindings())
            std::printf("bindings saved to %s\n", bindings_path.c_str());
        else
            std::fprintf(stderr, "cannot write the bindings to %s\n", bindings_path.c_str());
    }

    // Applied live, so a rebound control works the moment it is bound rather than after a restart.
    // The file waits until the screen closes: a dead zone nudged five times is one save, not five.
    void apply_bindings() {
        if (!menu.dirty())
            return;
        menu.mark_saved();
        bindings_dirty = true;
        window.set_bindings(menu.bindings());
    }

    // A missing file is the ordinary first run, not a fault: the defaults are the layout the
    // launcher always had, so nothing needs to exist for the keyboard to work.
    void load_bindings() {
        dream::render::Bindings b = dream::render::Bindings::defaults();
        std::ifstream in(bindings_path, std::ios::binary);
        if (!bindings_path.empty() && in) {
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            std::string warnings;
            b = dream::render::Bindings::from_text(text, &warnings);
            if (!warnings.empty())
                std::fprintf(stderr, "%s:\n%s", bindings_path.c_str(), warnings.c_str());
            std::printf("bindings: %s\n", bindings_path.c_str());
        }
        window.set_bindings(b);
    }

    bool save_bindings() const {
        std::ofstream out(bindings_path, std::ios::binary);
        if (!out)
            return false;
        const std::string text = menu.bindings().to_text();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(out);
    }

    // Averaged over a window rather than taken frame to frame: an instantaneous figure on a
    // 60 Hz display flickers between two integers and is unreadable, which is the usual reason an
    // on-screen counter gets turned off again. Half a second is long enough to settle and short
    // enough to react to a stutter.
    void update_counter(std::uint64_t guest_cycles) {
        const auto now = std::chrono::steady_clock::now();
        const double guest_s = static_cast<double>(guest_cycles) / 200e6;
        if (fps_mark == std::chrono::steady_clock::time_point{}) {
            fps_mark = now;
            fps_frames_at_mark = presented;
            fps_guest_at_mark = guest_s;
            return;
        }
        const double elapsed = std::chrono::duration<double>(now - fps_mark).count();
        if (elapsed < 0.5)
            return;
        fps_shown = static_cast<double>(presented - fps_frames_at_mark) / elapsed;
        speed_shown = (guest_s - fps_guest_at_mark) / elapsed;
        fps_mark = now;
        fps_frames_at_mark = presented;
        fps_guest_at_mark = guest_s;
    }

    // Drawn into the frame the presenter is about to upload, so its size follows --scale and it
    // stays the same size relative to the picture however the window is resized.
    void draw_counter(std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
        char line[64];
        std::snprintf(line, sizeof line, "%.1f FPS  %.2fX", fps_shown, speed_shown);
        dream::render::TextStyle style;
        // One text pixel per two guest pixels at --scale 1, and the same apparent size at every
        // scale above it, so the counter neither disappears nor swamps the picture.
        style.scale = std::max(2u, 2u * (w / kGuestWidth));
        const int margin = static_cast<int>(4 * style.scale);
        dream::render::draw_text(px, w, w, h, margin, margin, line, style);
    }

    void screenshot() {
        char name[64];
        std::snprintf(name, sizeof name, "screenshot-%03u.ppm", screenshots++);
        screenshot_to(name);
        std::printf("wrote %s: presented frame %llu, guest frame %llu\n", name,
                    static_cast<unsigned long long>(presented),
                    static_cast<unsigned long long>(guest_frame));
    }

    void screenshot_to(const std::string& name) {
        // --screenshot-presented: what is actually on the screen, through the presenter, with the
        // overlays and the resolve. The default below is the game's own pixels instead, which is
        // what makes one screenshot comparable with another; this is for looking at the display
        // path itself, which nothing else can see.
        if (shot_presented) {
            std::vector<std::uint8_t> px;
            std::uint32_t w = 0, h = 0;
            if (window.read_pixels(px, w, h)) {
                if (FILE* pf = std::fopen(name.c_str(), "wb")) {
                    std::fprintf(pf, "P6\n%u %u\n255\n", w, h);
                    std::fwrite(px.data(), 1, px.size(), pf);
                    std::fclose(pf);
                }
                return;
            }
            std::fprintf(stderr, "could not read the presented image back\n");
            return;
        }
        FILE* f = std::fopen(name.c_str(), "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", shown.width, shown.height);
        const std::uint32_t* src = from_renderer ? offscreen.pixels() : pixels.data();
        const std::size_t n = static_cast<std::size_t>(shown.width) * shown.height;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t p = src[i];
            const unsigned char rgb[3] = {static_cast<unsigned char>(p),
                                          static_cast<unsigned char>(p >> 8),
                                          static_cast<unsigned char>(p >> 16)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }

    // Everything needed to reproduce what is on screen right now, in one keypress: the picture, the
    // display list that drew it, video memory and the PVR registers from the same instant, and the
    // numbers that say where the run had got to. A report that comes with one of these can be
    // rendered again offline with dream_render_view; a report without one is a description of a
    // picture nobody else can see.
    void capture() {
        char stem[64];
        std::snprintf(stem, sizeof stem, "capture-%03u", captures++);
        screenshot_to(std::string(stem) + ".ppm");
        if (!last_stream.empty()) {
            if (FILE* f = std::fopen((std::string(stem) + ".ta").c_str(), "wb")) {
                std::fwrite(last_stream.data(), 4, last_stream.size(), f);
                std::fclose(f);
            }
        }
        if (FILE* f = std::fopen((std::string(stem) + ".vram").c_str(), "wb")) {
            std::fwrite(memory_.vram(), 1, dream::mem::DcMemory::kVramSize, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((std::string(stem) + ".vram.regs").c_str(), "wb")) {
            std::fwrite(pvr_.reg_block(), 4, 0x2000 / 4, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((std::string(stem) + ".txt").c_str(), "w")) {
            std::fprintf(f, "guest frame %llu, presented frame %llu\n",
                         static_cast<unsigned long long>(guest_frame),
                         static_cast<unsigned long long>(presented));
            std::fprintf(f, "renders %llu, backgrounds %llu, written back %llu\n",
                         static_cast<unsigned long long>(rendered),
                         static_cast<unsigned long long>(backgrounds),
                         static_cast<unsigned long long>(written_back));
            std::fprintf(f, "textures: %u decoded, %u failed, %u overwritten\n",
                         renderer.textures().decoded, renderer.textures().failed,
                         renderer.textures().overwritten);
            std::fprintf(f, "display: %s\n", shown.describe().c_str());
            std::fprintf(f, "source: %s\n", from_renderer ? "the renderer" : "video memory");
            std::fprintf(f, "parameter words: %zu\n", last_stream.size());
            std::fclose(f);
        }
        std::printf("captured %s.{ppm,ta,vram,vram.regs,txt} at guest frame %llu\n", stem,
                    static_cast<unsigned long long>(guest_frame));
    }

    dream::render::PaletteFormat palette_format() const {
        return static_cast<dream::render::PaletteFormat>(pvr_.reg(0x108) & 3u);
    }

    static unsigned bytes_per_written_pixel(const dream::render::FramebufferInfo& info) {
        switch (info.format) {
            case dream::render::FramebufferFormat::Rgb888:
                return 3;
            case dream::render::FramebufferFormat::Argb8888:
                return 4;
            default:
                return 2;
        }
    }

    static constexpr std::uint32_t kGuestWidth = 640, kGuestHeight = 480;

    dream::render::vk::Window window;
    dream::render::vk::Renderer renderer;
    dream::render::vk::Offscreen offscreen;
    dream::render::vk::Presenter presenter;
    dream::render::DisplayList decoder;
    dream::render::vk::FrameGeometry geometry;
    dream::render::FramebufferInfo shown;
    std::vector<std::uint32_t> pixels;
    std::string error;
    std::uint64_t rendered = 0, written_back = 0, presented = 0, palette_changes = 0,
                  decoded_frames = 0, backgrounds = 0;
    std::uint32_t target_address = 0;
    std::uint64_t guest_frame = 0;
    std::vector<std::uint32_t> last_stream;
    unsigned screenshots = 0, captures = 0;
    // The on-screen counter. Two numbers, because they answer different questions: how smooth it
    // looks (frames actually presented per second of wall clock) and whether it is keeping up
    // (guest time elapsed per second of wall clock, where 1.00x is the console's own pace).
    bool show_fps = false;
    // The binding screen (F1, or a pad's select button). The guest is stopped while it is up, so
    // nobody rebinds a control mid-corner.
    dream::render::InputMenu menu;
    // Where a change is written back. Empty means this run only: --bindings was pointed nowhere
    // usable, or SDL could not name a settings directory on this host.
    std::string bindings_path;
    bool bindings_dirty = false;  // changed since the screen was opened
    // Escape closes the screen and also quits: quit stays disarmed until the key comes back up.
    bool rearm_escape = false;
    // --screenshot-at N / --capture-at N: the same thing F12 and F11 do, at the Nth presented
    // frame, for a run with nobody at the keyboard. These were environment variables, which is
    // fine for a one-off and wrong for something the documentation tells people to use.
    std::uint64_t screenshot_at = 0, capture_at = 0;
    std::chrono::steady_clock::time_point fps_mark{};
    std::uint64_t fps_frames_at_mark = 0;
    double fps_guest_at_mark = 0;
    double fps_shown = 0, speed_shown = 0;
    bool writeback = false;       // --framebuffer-writeback
    bool shot_presented = false;  // --screenshot-presented
    bool have_frame = false, from_renderer = false, reported_error = false;

private:
    dream::mem::DcMemory& memory_;
    const dream::pvr::Core& pvr_;
    std::vector<std::uint32_t> targets_;  // every buffer the renderer has drawn into
};

}  // namespace port
