// Draws decoded display lists with Metal. Each render draws into the target of the framebuffer
// that FB_W_SOF1 names. The console scans out one buffer while it renders the other, and shows each
// render a frame after it is ready here, so the window shows the newest render, the picture that
// the display shows next. Screenshots read the target that FB_R_SOF1 names. The translucent
// polygons of each pixel blend from the farthest to the nearest, as the PowerVR's autosort mode
// does. Polygons that only add to the pixel, as most of an explosion, give the same pixel in any
// order, so they are summed instead of sorted, and the GPU's own blending adds them where nothing
// else lies on the pixel. The bloom changes only what the window shows, not the targets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/draw.h"

struct SDL_Window;

namespace render {

class Gpu {
public:
    Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;
    ~Gpu();

    // `window` may be null for a run without one; one given must have been made with
    // SDL_WINDOW_METAL. `scale` multiplies the 640x480 the geometry is drawn at.
    bool create(SDL_Window* window, unsigned scale, std::string& error);
    // Draws a frame into its target. Its textures come from `hw`'s video memory.
    void render(const Frame& frame, const Hardware& hw);
    // Draws the frame that translate() makes of `list`.
    void render(const DisplayList& list, const Hardware& hw);
    // Shows the newest render in the window, letterboxed to 4:3, and over it the overlay
    // given last, premultiplied RGBA at the window's pixel size. Null pixels show none. A framebuffer
    // larger than the window's picture is averaged over each of its pixels. The GPU draws the
    // picture after the render, so the CPU does not wait for it: a render that the GPU has finished
    // is checked first and drawn again if it overflowed the pool, and one that is still drawing
    // shows as it comes. With the display's sync
    // on, `refresh` other than 0 is the display's refresh that shows the picture, a time of now()'s
    // clock. `white` shows a white picture instead (IKARUGA_LATENCY, window.h), and `mark` other
    // than 0 is a time that the latency report pairs with the refresh that showed the picture.
    void present(const Hardware& hw, double refresh = 0, bool white = false, double mark = 0);
    // Presents wait for the display's refreshes, which a run paced by the display needs (window.h).
    void set_display_sync(bool on);
    // Metal's performance HUD over the window: the frame rate, the frame times and the GPU's time. It
    // shows only in a run that set MTL_HUD_ENABLED before the first Metal device (main.cpp).
    void set_hud(bool on);
    // The frames' times from their start to the end of their render on the GPU: start_frame() gives
    // the next render's start, and take_work() a finished render's time once, or 0.
    void start_frame(double t);
    double take_work();
    // The last refresh that brought a picture on the screen, 0 before the first, and the presents
    // whose picture came later than the refresh they asked for.
    double presented() const;
    uint64_t late() const;
    double refresh_period() const;  // of the display that shows the window, in seconds
    static double now();            // the host's clock in seconds
    // The renders from now on are 640x480 times `scale`. The framebuffers so far are scaled to it, so
    // the window shows the last frame while the game stops.
    void set_scale(unsigned scale);
    unsigned scale() const;
    // How present() turns the picture in the window, for a monitor stood on its side: 0 as it is, 1 a
    // quarter turn counter-clockwise, 2 a quarter turn clockwise. The turned picture is letterboxed
    // to 3:4. The overlay is not turned: it is drawn for the window as the player sees it. Only the
    // Vulkan renderer turns the picture so far.
    void set_rotation(unsigned turn) { rotation_ = turn; }
    unsigned rotation() const { return rotation_; }
    // The bloom over the picture in the window: 0 none, 1 low, 2 high.
    void set_bloom(unsigned level);
    // The samples of the anisotropic filtering of bilinear textures, from 1 (none) to 16, for the
    // renders from now on.
    void set_anisotropy(unsigned samples);
    // ENGLISH TEXT: English in the textures that hold Japanese words (english_textures.h).
    void set_english(bool on);
    unsigned anisotropy() const;
    void set_overlay(const uint32_t* pixels, uint32_t width, uint32_t height);
    // The displayed framebuffer as RGBA8, red in the lowest byte. False if no render made it.
    // `bloomed` adds the bloom, as the window shows the framebuffer before scaling it.
    bool read_displayed(const Hardware& hw, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height,
                        bool bloomed = false);
    // The framebuffer at an address in the 32-bit view, as FB_W_SOF1 names it.
    bool read_target(uint32_t address, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height,
                     bool bloomed = false);
    // A cached texture's full-size level as the GPU holds it, for tests.
    bool read_texture(uint32_t tcw, uint32_t tsp, uint32_t palette_format, std::vector<uint32_t>& pixels,
                      uint32_t& width, uint32_t& height);
    const char* driver() const;  // the GPU's name
    // The last render's GPU time and its translucent fragments, which the pixels' lists held.
    void last_render(double& gpu_ms, uint32_t& fragments);

    uint64_t renders = 0, textures_decoded = 0;
    // Strips drawn without their texture, which the decoder cannot read: shown once a texture
    // control word with IKARUGA_RENDER_DEBUG.
    uint64_t untextured = 0;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    unsigned rotation_ = 0;
};

}  // namespace render
