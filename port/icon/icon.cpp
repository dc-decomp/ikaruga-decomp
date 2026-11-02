// Draws Ikaruga.app's icon into an iconset directory, for iconutil (CMakeLists.txt).
//
//   ikaruga-icon DIR.iconset
//
// The icon is the port's own picture, not the game's art: a squircle split into a dark and a light
// half, with one ship-like arrow across the split in the other half's color, as the game's two
// polarities. Each size is drawn from the vectors, so the small ones stay sharp.
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

// The canvas is 1024 units wide. The squircle fills Apple's icon grid: 824 units, 100 from each edge.
constexpr double kCanvas = 1024, kHalf = 412, kCenter = 512;

CGColorRef rgb(CGColorSpaceRef space, double r, double g, double b, double a = 1) {
    const CGFloat c[4] = {CGFloat(r), CGFloat(g), CGFloat(b), CGFloat(a)};
    return CGColorCreate(space, c);
}

// A superellipse of exponent 5, close to the continuous corners of macOS's icon shape.
CGMutablePathRef squircle() {
    CGMutablePathRef path = CGPathCreateMutable();
    constexpr int kSteps = 720;
    for (int i = 0; i < kSteps; ++i) {
        const double t = 2 * M_PI * i / kSteps, c = std::cos(t), s = std::sin(t);
        const double x = kCenter + kHalf * std::copysign(std::pow(std::fabs(c), 0.4), c);
        const double y = kCenter + kHalf * std::copysign(std::pow(std::fabs(s), 0.4), s);
        i ? CGPathAddLineToPoint(path, nullptr, x, y) : CGPathMoveToPoint(path, nullptr, x, y);
    }
    CGPathCloseSubpath(path);
    return path;
}

// The dark half: left of a slanted line through the squircle's top and bottom.
CGMutablePathRef dark_half() {
    CGMutablePathRef path = CGPathCreateMutable();
    const double top = kCenter + 90, bottom = kCenter - 90, slope = (top - bottom) / (2 * kHalf);
    CGPathMoveToPoint(path, nullptr, -1, -1);
    CGPathAddLineToPoint(path, nullptr, bottom - slope * (kCenter - kHalf + 1), -1);
    CGPathAddLineToPoint(path, nullptr, top + slope * (kCenter - kHalf + 1), kCanvas + 1);
    CGPathAddLineToPoint(path, nullptr, -1, kCanvas + 1);
    CGPathCloseSubpath(path);
    return path;
}

// The ship, pointing up: a narrow nose, swept wings and a notched tail, the same on both sides.
CGMutablePathRef ship() {
    // From the nose down the right side; the left side mirrors it.
    static const double kRight[][2] = {{0, 830}, {44, 590}, {230, 300}, {64, 372}, {46, 292}, {0, 340}};
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathMoveToPoint(path, nullptr, kCenter, kRight[0][1]);
    for (const auto& p : kRight)
        CGPathAddLineToPoint(path, nullptr, kCenter + p[0], p[1]);
    for (int i = 4; i > 0; --i)
        CGPathAddLineToPoint(path, nullptr, kCenter - kRight[i][0], kRight[i][1]);
    CGPathCloseSubpath(path);
    return path;
}

void gradient(CGContextRef cg, CGColorSpaceRef space, const CGFloat (&top)[4], const CGFloat (&bottom)[4]) {
    const CGFloat colors[8] = {top[0], top[1], top[2], top[3], bottom[0], bottom[1], bottom[2], bottom[3]};
    CGGradientRef g = CGGradientCreateWithColorComponents(space, colors, nullptr, 2);
    CGContextDrawLinearGradient(cg, g, CGPointMake(0, kCenter + kHalf), CGPointMake(0, kCenter - kHalf), 0);
    CGGradientRelease(g);
}

void draw(CGContextRef cg, CGColorSpaceRef space) {
    CGMutablePathRef shape = squircle(), dark = dark_half(), arrow = ship();

    CGContextSaveGState(cg);
    CGColorRef shadow = rgb(space, 0, 0, 0, 0.35);
    CGContextSetShadowWithColor(cg, CGSizeMake(0, -10), 24, shadow);
    CGColorRef base = rgb(space, 0.5, 0.5, 0.5);
    CGContextSetFillColorWithColor(cg, base);
    CGContextAddPath(cg, shape);
    CGContextFillPath(cg);
    CGContextRestoreGState(cg);

    CGContextSaveGState(cg);
    CGContextAddPath(cg, shape);
    CGContextClip(cg);
    gradient(cg, space, {0.97, 0.97, 0.98, 1}, {0.83, 0.85, 0.89, 1});
    CGContextAddPath(cg, dark);
    CGContextClip(cg);
    gradient(cg, space, {0.10, 0.12, 0.19, 1}, {0.02, 0.03, 0.05, 1});
    CGContextRestoreGState(cg);

    // The ship's light part on the dark half glows blue, its dark part on the light half red.
    for (const bool on_dark : {true, false}) {
        CGContextSaveGState(cg);
        CGContextAddPath(cg, shape);
        CGContextClip(cg);
        if (on_dark) {
            CGContextAddPath(cg, dark);
            CGContextClip(cg);
        } else {
            CGContextAddPath(cg, dark);
            CGContextAddRect(cg, CGRectMake(-1, -1, kCanvas + 2, kCanvas + 2));
            CGContextEOClip(cg);
        }
        CGColorRef glow = on_dark ? rgb(space, 0.35, 0.62, 1.0, 0.9) : rgb(space, 1.0, 0.18, 0.16, 0.7);
        CGColorRef fill = on_dark ? rgb(space, 0.96, 0.97, 0.99) : rgb(space, 0.04, 0.05, 0.08);
        CGContextSetShadowWithColor(cg, CGSizeZero, 40, glow);
        CGContextSetFillColorWithColor(cg, fill);
        CGContextAddPath(cg, arrow);
        CGContextFillPath(cg);
        CGColorRelease(glow);
        CGColorRelease(fill);
        CGContextRestoreGState(cg);
    }

    CGColorRelease(shadow);
    CGColorRelease(base);
    CGPathRelease(shape);
    CGPathRelease(dark);
    CGPathRelease(arrow);
}

bool write(const std::string& path, int pixels) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(nullptr, pixels, pixels, 8, 0, space, kCGImageAlphaPremultipliedLast);
    CGContextScaleCTM(cg, pixels / kCanvas, pixels / kCanvas);
    draw(cg, space);
    CGImageRef image = CGBitmapContextCreateImage(cg);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), false);
    CGImageDestinationRef out = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = out != nullptr;
    if (out) {
        CGImageDestinationAddImage(out, image, nullptr);
        ok = CGImageDestinationFinalize(out);
        CFRelease(out);
    }
    CFRelease(url);
    CGImageRelease(image);
    CGContextRelease(cg);
    CGColorSpaceRelease(space);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ikaruga-icon DIR.iconset\n");
        return 2;
    }
    const std::string dir = argv[1];
    for (const int points : {16, 32, 128, 256, 512})
        for (const int scale : {1, 2}) {
            const std::string name = "icon_" + std::to_string(points) + "x" + std::to_string(points) +
                                     (scale == 2 ? "@2x" : "") + ".png";
            if (!write(dir + "/" + name, points * scale)) {
                std::fprintf(stderr, "ikaruga-icon: cannot write %s/%s\n", dir.c_str(), name.c_str());
                return 1;
            }
        }
    return 0;
}
