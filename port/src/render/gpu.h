// Draws decoded display lists with Metal. Each render draws into the target of the framebuffer
// that FB_W_SOF1 names, and the window shows the target that FB_R_SOF1 names, as the console scans
// out one buffer while it renders the other. The translucent polygons of each pixel blend from the
// farthest to the nearest, as the PowerVR's autosort mode does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/ta.h"

struct SDL_Window;

namespace render {

// What a render reads besides the display list: the PowerVR's registers (0x2000 bytes from
// 0x005F8000, palette memory and the fog table included) and video memory in its 64-bit view.
struct Hardware {
    const uint32_t* registers = nullptr;
    const uint8_t* vram = nullptr;
    size_t vram_size = 0;
};

class Gpu {
public:
    Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;
    ~Gpu();

    // `window` may be null for a run without one; one given must have been made with
    // SDL_WINDOW_METAL. `scale` multiplies the 640x480 the geometry is drawn at.
    bool create(SDL_Window* window, unsigned scale, std::string& error);
    void render(const DisplayList& list, const Hardware& hw);
    // Shows the displayed framebuffer in the window, letterboxed to 4:3, and over it the overlay
    // given last, premultiplied RGBA at the window's pixel size. Null pixels show none.
    void present(const Hardware& hw);
    void set_overlay(const uint32_t* pixels, uint32_t width, uint32_t height);
    // The displayed framebuffer as RGBA8, red in the lowest byte. False if no render made it.
    bool read_displayed(const Hardware& hw, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height);
    // The framebuffer at an address in the 32-bit view, as FB_W_SOF1 names it.
    bool read_target(uint32_t address, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height);
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
};

}  // namespace render
