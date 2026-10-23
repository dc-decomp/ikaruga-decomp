// A picture over the game in the window: boxes and text for the F1 screen and the frame counter,
// at the window's own pixel size, so text stays sharp at any scale. Drawn with Core Text into a
// premultiplied RGBA bitmap, which the renderer blends over the frame.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace render {

class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Starts a new picture, all transparent. Coordinates are pixels from the top left.
    void begin(uint32_t width, uint32_t height);
    // `rgba` is red in the lowest byte, alpha in the highest, not premultiplied.
    void box(float x, float y, float w, float h, uint32_t rgba);
    // `y` is the top of the text's line. Returns the text's width.
    float text(float x, float y, const std::string& s, float size, uint32_t rgba);
    float text_width(const std::string& s, float size);

    bool empty() const { return !active_; }
    const std::vector<uint32_t>& pixels() const { return pixels_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // Nothing to show until the next begin(). The bitmap stays, as the context draws into it.
    void clear() { active_ = false; }

private:
    std::vector<uint32_t> pixels_;
    uint32_t width_ = 0, height_ = 0;
    bool active_ = false;
    void* context_ = nullptr;  // the CGContext over pixels_
};

}  // namespace render
