#include "render/overlay.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

namespace render {
namespace {

CGColorRef colour(uint32_t rgba) {
    const CGFloat c[4] = {(rgba & 255) / 255.0, ((rgba >> 8) & 255) / 255.0, ((rgba >> 16) & 255) / 255.0,
                          (rgba >> 24) / 255.0};
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGColorRef out = CGColorCreate(space, c);
    CGColorSpaceRelease(space);
    return out;
}

CTLineRef line_of(const std::string& s, float size, uint32_t rgba) {
    CTFontRef font = CTFontCreateWithName(CFSTR("Menlo-Bold"), size, nullptr);
    CGColorRef c = colour(rgba);
    CFStringRef text = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()),
                                               static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* values[] = {font, c};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed = CFAttributedStringCreate(nullptr, text ? text : CFSTR("?"), attributes);
    CTLineRef line = CTLineCreateWithAttributedString(attributed);
    CFRelease(attributed);
    CFRelease(attributes);
    if (text)
        CFRelease(text);
    CGColorRelease(c);
    CFRelease(font);
    return line;
}

}  // namespace

Overlay::Overlay() = default;

Overlay::~Overlay() {
    if (context_)
        CGContextRelease(static_cast<CGContextRef>(context_));
}

void Overlay::begin(uint32_t width, uint32_t height) {
    if (width != width_ || height != height_ || !context_) {
        if (context_)
            CGContextRelease(static_cast<CGContextRef>(context_));
        width_ = width;
        height_ = height;
        pixels_.assign(static_cast<size_t>(width) * height, 0);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        const uint32_t layout = static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
                                static_cast<uint32_t>(kCGBitmapByteOrder32Big);
        context_ = CGBitmapContextCreate(pixels_.data(), width, height, 8, width * 4, space, layout);
        CGColorSpaceRelease(space);
        // The bitmap's first row is the top of the window.
        CGContextTranslateCTM(static_cast<CGContextRef>(context_), 0, height);
        CGContextScaleCTM(static_cast<CGContextRef>(context_), 1, -1);
    }
    std::fill(pixels_.begin(), pixels_.end(), 0u);
    active_ = true;
}

void Overlay::box(float x, float y, float w, float h, uint32_t rgba) {
    if (!context_)
        return;
    CGContextRef cg = static_cast<CGContextRef>(context_);
    CGColorRef c = colour(rgba);
    CGContextSetFillColorWithColor(cg, c);
    CGContextFillRect(cg, CGRectMake(x, y, w, h));
    CGColorRelease(c);
}

float Overlay::text_width(const std::string& s, float size) {
    CTLineRef line = line_of(s, size, 0xFFFFFFFFu);
    const double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line);
    return static_cast<float>(w);
}

float Overlay::text(float x, float y, const std::string& s, float size, uint32_t rgba) {
    if (!context_)
        return 0;
    CGContextRef cg = static_cast<CGContextRef>(context_);
    CTLineRef line = line_of(s, size, rgba);
    CGFloat ascent = 0;
    const double w = CTLineGetTypographicBounds(line, &ascent, nullptr, nullptr);
    // Core Text draws upright in a context whose y points up: flip back around the baseline.
    CGContextSaveGState(cg);
    CGContextTranslateCTM(cg, x, y + ascent);
    CGContextScaleCTM(cg, 1, -1);
    CGContextSetTextPosition(cg, 0, 0);
    CTLineDraw(line, cg);
    CGContextRestoreGState(cg);
    CFRelease(line);
    return static_cast<float>(w);
}

}  // namespace render
