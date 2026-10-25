// The renderer in Metal. See gpu.h, and shaders/geometry.metal for the drawing itself.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "render/gpu.h"
#include "render/shaders.h"
#include "render/texture.h"

namespace render {
namespace {

// FB_R_SOF1, the frame buffer on display, as a word offset from the PowerVR's registers.
constexpr size_t kFbRSof1 = 0x050 / 4;

// The shaders' structures (shaders/geometry.metal).
struct Screen {
    float scale[4];
};
struct DrawUniforms {
    int32_t flags, shading, fog;
    uint32_t blend;
    float alpha_ref;
    uint32_t order;
    float lod_bias, pad;
};
struct FogUniforms {
    float table_colour[4];
    float vertex_colour[4];
    float params[4];
    float table[64][4];
};
struct Lists {
    uint32_t width, capacity;
};
constexpr size_t kNodeBytes = 16;
// The screen is drawn in bands of rows, one after another, which all use the one pool of
// translucent fragments. A band's fragments are sorted and blended before the next band's fill the
// pool, as the PowerVR finishes a tile before the next. Bands are whole slices of kSliceRows rows,
// whose fragments the shaders count.
constexpr size_t kMaxBands = 128;
constexpr uint32_t kSliceRows = 16;
struct Band {
    uint32_t y0, y1;    // its rows, the last excluded
    uint64_t estimate;  // the translucent fragments the last render had in these rows
};

// Bands whose translucent fragments fit `budget` each, by what each slice needed in the last render.
std::vector<Band> plan_bands(const uint32_t* slices, uint32_t count, uint32_t budget) {
    std::vector<Band> out;
    uint32_t start = 0;
    uint64_t sum = 0;
    for (uint32_t s = 0; s < count; ++s) {
        if (s > start && sum + slices[s] > budget) {
            out.push_back({start * kSliceRows, s * kSliceRows, sum});
            start = s;
            sum = 0;
        }
        sum += slices[s];
    }
    out.push_back({start * kSliceRows, count * kSliceRows, sum});
    return out;
}

uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h);

// A texture's place in the cache: its format, and with a palette the palette's format.
uint64_t texture_key(const TextureFormat& t, uint32_t palette_format) {
    const uint32_t fields[] = {t.address, t.width, t.height, static_cast<uint32_t>(t.pixel), t.twiddled, t.vq,
                               t.mipmapped, t.row_texels, t.palette_base, palette_format};
    return hash_bytes(reinterpret_cast<const uint8_t*>(fields), sizeof fields, 0);
}

uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h) {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * k;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * k;
    return h ^ (h >> 32);
}

// The blend factors of the TSP word. The source's "other" colour is the destination's and the other
// way round.
MTLBlendFactor blend_factor(uint32_t instruction, bool source) {
    switch (instruction & 7) {
        case 0: return MTLBlendFactorZero;
        case 1: return MTLBlendFactorOne;
        case 2: return source ? MTLBlendFactorDestinationColor : MTLBlendFactorSourceColor;
        case 3: return source ? MTLBlendFactorOneMinusDestinationColor : MTLBlendFactorOneMinusSourceColor;
        case 4: return MTLBlendFactorSourceAlpha;
        case 5: return MTLBlendFactorOneMinusSourceAlpha;
        case 6: return MTLBlendFactorDestinationAlpha;
        default: return MTLBlendFactorOneMinusDestinationAlpha;
    }
}

// The ISP word's depth compare, on 1/w: larger is nearer.
MTLCompareFunction compare(uint32_t mode) {
    constexpr MTLCompareFunction ops[8] = {MTLCompareFunctionNever,        MTLCompareFunctionLess,
                                           MTLCompareFunctionEqual,        MTLCompareFunctionLessEqual,
                                           MTLCompareFunctionGreater,      MTLCompareFunctionNotEqual,
                                           MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    return ops[mode & 7];
}

void colour_of(uint32_t reg, float (&out)[4]) {
    out[0] = static_cast<float>((reg >> 16) & 255) / 255.0f;
    out[1] = static_cast<float>((reg >> 8) & 255) / 255.0f;
    out[2] = static_cast<float>(reg & 255) / 255.0f;
    out[3] = 1.0f;
}

// A strip's triangles as the GPU draws them.
struct Batch {
    uint32_t first, count;
    bool translucent;
    uint32_t pipeline;  // an opaque or punch-through strip's blend factors
    uint32_t depth;     // the depth compare in bits 0-2, depth writes in bit 3
    id<MTLTexture> texture;
    uint32_t sampler;
    MTLScissorRect scissor;
    DrawUniforms uniforms;
};

}  // namespace

struct Gpu::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    SDL_Window* window = nullptr;
    SDL_MetalView view = nullptr;
    CAMetalLayer* layer = nil;
    MTLVertexDescriptor* vertex_layout = nil;
    std::map<uint32_t, id<MTLRenderPipelineState>> pipelines;
    id<MTLRenderPipelineState> translucent = nil, sort = nil, present = nil, overlay = nil;
    id<MTLTexture> overlays[2] = {nil, nil};  // written in turn, so the CPU never writes one in use
    int overlay_next = 0;
    id<MTLTexture> overlay_shown = nil;
    std::map<uint32_t, id<MTLDepthStencilState>> depth_states;
    std::map<uint32_t, id<MTLSamplerState>> samplers;
    id<MTLSamplerState> present_sampler = nil;
    std::map<uint32_t, id<MTLTexture>> targets;  // by framebuffer address
    id<MTLTexture> depth = nil, white = nil;
    struct Texture {
        id<MTLTexture> texture = nil;
        uint64_t hash = 0;
        uint64_t used = 0;  // the render that last used it
    };
    std::unordered_map<uint64_t, Texture> textures;
    id<MTLBuffer> vertices = nil;
    // The pool's nodes are counted out to each band in turn; slices: the fragments each slice of
    // rows needed in the last render.
    id<MTLBuffer> heads = nil, nodes = nil, counters = nil, slices = nil;
    uint32_t node_capacity = 0;
    std::vector<Band> bands;  // the last render's
    id<MTLCommandBuffer> last = nil;  // the last render's
    // What the last render needs to be drawn again when its translucent fragments overflow the pool.
    // The GPU draws a render while the CPU runs the next frame, and the next render checks it first.
    struct Pending {
        bool open = false;
        int attempts = 0;
        uint32_t target = 0;
        MTLClearColor background{};
        float background_depth = 0, z_scale = 1;
        FogUniforms fog{};
    } pending;
    unsigned scale = 1;
    uint32_t width = 640, height = 480;
    std::vector<Vertex> triangles;
    std::vector<uint32_t> texels;
    std::vector<Batch> draws;

    void wait() {
        if (last)
            [last waitUntilCompleted];
    }

    // The last render complete. A band that overflowed the pool dropped fragments, as an explosion's
    // first frame can need 40 times the fragments of the render before. The render is then drawn again
    // with its bands planned from what each slice needed, up to three times.
    void finish() {
        wait();
        while (pending.open) {
            bool overflowed = false;
            const auto* counts = static_cast<const uint32_t*>(counters.contents);
            for (size_t b = 0; b < bands.size(); ++b)
                if (counts[b] > node_capacity) {
                    overflowed = true;
                    if (std::getenv("IKARUGA_RENDER_DEBUG"))
                        std::fprintf(stderr,
                                     "render: rows %u to %u needed %u translucent fragments, and %u fit; the "
                                     "render before had %llu there\n",
                                     bands[b].y0, bands[b].y1 - 1, counts[b], node_capacity,
                                     static_cast<unsigned long long>(bands[b].estimate));
                }
            if (!overflowed || pending.attempts == 3) {
                pending.open = false;
                break;
            }
            ++pending.attempts;
            encode();
            wait();
        }
    }

    // The pending render's draws, into a command buffer that the GPU runs while the CPU goes on.
    void encode();

    id<MTLRenderPipelineState> make(NSString* vertex, NSString* fragment, MTLPixelFormat colour, bool depth_attachment,
                                    void (^configure)(MTLRenderPipelineColorAttachmentDescriptor*)) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library newFunctionWithName:vertex];
        d.fragmentFunction = [library newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = colour;
        if (configure)
            configure(d.colorAttachments[0]);
        if (depth_attachment) {
            d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            d.vertexDescriptor = vertex_layout;
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&error];
        if (!p)
            std::fprintf(stderr, "render: pipeline %s/%s: %s\n", vertex.UTF8String, fragment.UTF8String,
                         error.localizedDescription.UTF8String);
        return p;
    }

    // One pipeline for each pair of blend factors.
    id<MTLRenderPipelineState> pipeline(uint32_t key) {
        if (auto it = pipelines.find(key); it != pipelines.end())
            return it->second;
        const uint32_t src = key & 7, dst = (key >> 3) & 7;
        id<MTLRenderPipelineState> p = make(@"geometry_vertex", @"geometry_fragment", MTLPixelFormatRGBA8Unorm, true,
                                            ^(MTLRenderPipelineColorAttachmentDescriptor* c) {
                                                c.blendingEnabled = !(src == 1 && dst == 0);
                                                c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = blend_factor(src, true);
                                                c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor =
                                                    blend_factor(dst, false);
                                            });
        pipelines[key] = p;
        return p;
    }

    id<MTLDepthStencilState> depth_state(uint32_t key) {
        if (auto it = depth_states.find(key); it != depth_states.end())
            return it->second;
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = compare(key & 7);
        d.depthWriteEnabled = (key >> 3) & 1;
        id<MTLDepthStencilState> s = [device newDepthStencilStateWithDescriptor:d];
        depth_states[key] = s;
        return s;
    }

    // One sampler for each filter and each way the coordinates leave the texture.
    id<MTLSamplerState> sampler(uint32_t key) {
        if (auto it = samplers.find(key); it != samplers.end())
            return it->second;
        const bool bilinear = key & 1, mipmapped = key & 2, trilinear = key & 4;
        auto address = [](bool clamp, bool flip) {
            return clamp ? MTLSamplerAddressModeClampToEdge
                         : flip ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
        };
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        d.minFilter = d.magFilter = bilinear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = !mipmapped ? MTLSamplerMipFilterNotMipmapped
                                 : trilinear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        d.sAddressMode = address(key & 8, key & 32);
        d.tAddressMode = address(key & 16, key & 64);
        id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:d];
        samplers[key] = s;
        return s;
    }

    id<MTLTexture> target(uint32_t address) {
        if (auto it = targets.find(address); it != targets.end())
            return it->second;
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        targets[address] = t;
        return t;
    }

    // Room for `capacity` translucent fragments.
    void lists(uint32_t capacity) {
        node_capacity = capacity;
        nodes = [device newBufferWithLength:static_cast<NSUInteger>(capacity) * kNodeBytes
                                    options:MTLResourceStorageModePrivate];
    }

    // The texture of a draw, decoded again when the bytes it occupies have changed since its last use.
    id<MTLTexture> texture(const render::Draw& d, const Frame& frame, const Hardware& hw, uint64_t render,
                           uint64_t& decoded, uint64_t& untextured) {
        Impl& m = *this;
        const TextureFormat& f = d.texture;
        const uint32_t* palette = frame.palette;
        auto without = [&](const char* why) {
            static std::vector<uint32_t> shown;
            ++untextured;
            if (std::getenv("IKARUGA_RENDER_DEBUG") && std::find(shown.begin(), shown.end(), f.address) == shown.end()) {
                shown.push_back(f.address);
                std::fprintf(stderr, "render: the texture at %06x drawn without itself: %s\n", f.address, why);
            }
            return m.white;
        };
        if (!d.texture_readable)
            return without("an encoding the decoder does not read");
        Texture& t = m.textures[texture_key(f, frame.palette_format)];
        if (t.texture && t.used == render)
            return t.texture;
        uint32_t first, size;
        texture_extent(f, first, size);
        if (static_cast<size_t>(first) + size > hw.vram_size)
            return without("outside video memory");
        uint64_t h = hash_bytes(hw.vram + first, size, 0);
        if (f.pixel == PixelFormat::Palette4 || f.pixel == PixelFormat::Palette8) {
            const uint32_t entries = f.pixel == PixelFormat::Palette4 ? 16 : 256;
            h = hash_bytes(reinterpret_cast<const uint8_t*>(palette + f.palette_base), entries * 4, h);
        }
        t.used = render;
        if (t.texture && t.hash == h)
            return t.texture;
        if (!t.texture) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:f.width
                                                                                        height:f.height
                                                                                     mipmapped:NO];
            d.mipmapLevelCount = f.levels();
            d.storageMode = MTLStorageModeShared;
            t.texture = [m.device newTextureWithDescriptor:d];
            if (!t.texture)
                return without("Metal has no room for it");
        }
        t.hash = h;
        ++decoded;
        for (uint32_t level = 0; level < f.levels(); ++level) {
            if (!decode_texture(f, level, hw.vram, hw.vram_size, palette, m.texels))
                break;
            const uint32_t w = std::max(1u, f.width >> level), hh = std::max(1u, f.height >> level);
            [t.texture replaceRegion:MTLRegionMake2D(0, 0, w, hh) mipmapLevel:level withBytes:m.texels.data()
                         bytesPerRow:w * 4];
        }
        return t.texture;
    }

    bool download(id<MTLTexture> t, uint32_t level, std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h) {
        finish();
        w = std::max<uint32_t>(1, static_cast<uint32_t>(t.width) >> level);
        h = std::max<uint32_t>(1, static_cast<uint32_t>(t.height) >> level);
        pixels.resize(static_cast<size_t>(w) * h);
        [t getBytes:pixels.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level];
        return true;
    }
};

void Gpu::Impl::encode() {
    Impl& m = *this;
    const Pending& pending = m.pending;
    const auto* slices = static_cast<const uint32_t*>(m.slices.contents);
    const uint32_t slice_count = m.height / kSliceRows;
    // Half the pool for each band, so that a band holds twice what the last render had there.
    std::vector<Band> bands = plan_bands(slices, slice_count, m.node_capacity / 2);
    while (bands.size() > kMaxBands) {
        m.lists(m.node_capacity * 2);
        bands = plan_bands(slices, slice_count, m.node_capacity / 2);
    }
    if (bands.size() != m.bands.size() && std::getenv("IKARUGA_RENDER_DEBUG"))
        std::fprintf(stderr, "render: %zu bands\n", bands.size());
    m.bands = bands;

    id<MTLCommandBuffer> cmd = [m.queue commandBuffer];
    // Every pixel's list starts empty, and so does each band's count.
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit fillBuffer:m.heads range:NSMakeRange(0, m.heads.length) value:0xFF];
    [blit fillBuffer:m.counters range:NSMakeRange(0, m.counters.length) value:0];
    [blit fillBuffer:m.slices range:NSMakeRange(0, m.slices.length) value:0];
    [blit endEncoding];

    id<MTLTexture> colour_target = m.target(pending.target);
    const Screen screen{{2.0f / 640.0f, 2.0f / 480.0f, 1.0f / pending.z_scale, 0.0f}};
    const Lists lists{m.width, m.node_capacity};
    for (size_t b = 0; b < m.bands.size(); ++b) {
        const Band& band = m.bands[b];
        // The band's geometry: each translucent fragment joins its pixel's list. Depth starts again in
        // each band, which reads only its own rows.
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = colour_target;
        pass.colorAttachments[0].loadAction = b == 0 ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.colorAttachments[0].clearColor = pending.background;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = m.depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.clearDepth = std::min(pending.background_depth / pending.z_scale, 1.0f);
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        if (!m.draws.empty()) {
            [enc setVertexBuffer:m.vertices offset:0 atIndex:0];
            [enc setVertexBytes:&screen length:sizeof screen atIndex:1];
            [enc setFragmentBytes:&pending.fog length:sizeof pending.fog atIndex:1];
            [enc setFragmentBytes:&lists length:sizeof lists atIndex:2];
            [enc setFragmentBuffer:m.heads offset:0 atIndex:3];
            [enc setFragmentBuffer:m.nodes offset:0 atIndex:4];
            [enc setFragmentBuffer:m.counters offset:b * 4 atIndex:5];
            [enc setFragmentBuffer:m.slices offset:0 atIndex:6];
        }
        id<MTLRenderPipelineState> bound = nil;
        for (const Batch& d : m.draws) {
            const NSUInteger y0 = std::max<NSUInteger>(d.scissor.y, band.y0);
            const NSUInteger y1 = std::min<NSUInteger>(d.scissor.y + d.scissor.height, band.y1);
            if (y1 <= y0)
                continue;
            id<MTLRenderPipelineState> p = d.translucent ? m.translucent : m.pipeline(d.pipeline);
            if (!p)
                continue;
            if (p != bound) {
                [enc setRenderPipelineState:p];
                bound = p;
            }
            [enc setDepthStencilState:m.depth_state(d.depth)];
            [enc setFragmentTexture:d.texture atIndex:0];
            [enc setFragmentSamplerState:m.sampler(d.sampler) atIndex:0];
            [enc setFragmentBytes:&d.uniforms length:sizeof d.uniforms atIndex:0];
            [enc setScissorRect:{d.scissor.x, y0, d.scissor.width, y1 - y0}];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:d.first vertexCount:d.count];
        }
        [enc endEncoding];

        // The band's translucent fragments, sorted and blended over its opaque picture.
        MTLRenderPassDescriptor* sort = [MTLRenderPassDescriptor renderPassDescriptor];
        sort.colorAttachments[0].texture = colour_target;
        sort.colorAttachments[0].loadAction = MTLLoadActionLoad;
        sort.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cmd renderCommandEncoderWithDescriptor:sort];
        [enc setRenderPipelineState:m.sort];
        [enc setFragmentBytes:&lists length:sizeof lists atIndex:2];
        [enc setFragmentBuffer:m.heads offset:0 atIndex:3];
        [enc setFragmentBuffer:m.nodes offset:0 atIndex:4];
        [enc setFragmentBuffer:m.slices offset:0 atIndex:5];
        [enc setScissorRect:{0, band.y0, m.width, band.y1 - band.y0}];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }
    [cmd commit];
    m.last = cmd;
}

Gpu::Gpu() : impl_(std::make_unique<Impl>()) {}

Gpu::~Gpu() {
    if (!impl_)
        return;
    impl_->wait();
    if (impl_->view)
        SDL_Metal_DestroyView(impl_->view);
}

bool Gpu::create(SDL_Window* window, unsigned scale, std::string& error) {
    Impl& m = *impl_;
    m.scale = std::max(1u, scale);
    m.width = 640 * m.scale;
    m.height = 480 * m.scale;
    m.device = MTLCreateSystemDefaultDevice();
    if (!m.device) {
        error = "no Metal device";
        return false;
    }
    m.queue = [m.device newCommandQueue];
    // The shaders are Metal source, compiled here: Xcode's offline Metal compiler is a separate
    // download, and compiling them takes a fraction of a second.
    NSString* source = [[NSString alloc] initWithBytes:shaders::geometry_metal
                                                length:shaders::geometry_metal_size
                                              encoding:NSUTF8StringEncoding];
    NSError* e = nil;
    m.library = [m.device newLibraryWithSource:source options:[MTLCompileOptions new] error:&e];
    if (!m.library) {
        error = std::string("the shaders do not compile: ") + e.localizedDescription.UTF8String;
        return false;
    }
    m.vertex_layout = [MTLVertexDescriptor vertexDescriptor];
    const MTLVertexFormat formats[4] = {MTLVertexFormatFloat3, MTLVertexFormatFloat2, MTLVertexFormatUChar4,
                                        MTLVertexFormatUChar4};
    const size_t offsets[4] = {offsetof(Vertex, x), offsetof(Vertex, u), offsetof(Vertex, base),
                               offsetof(Vertex, offset)};
    for (int i = 0; i < 4; ++i) {
        m.vertex_layout.attributes[i].format = formats[i];
        m.vertex_layout.attributes[i].offset = offsets[i];
        m.vertex_layout.attributes[i].bufferIndex = 0;
    }
    m.vertex_layout.layouts[0].stride = sizeof(Vertex);
    m.translucent = m.make(@"geometry_vertex", @"translucent_fragment", MTLPixelFormatRGBA8Unorm, true,
                           ^(MTLRenderPipelineColorAttachmentDescriptor* c) { c.writeMask = MTLColorWriteMaskNone; });
    m.sort = m.make(@"full_screen_vertex", @"sort_fragment", MTLPixelFormatRGBA8Unorm, false, nil);
    if (!m.translucent || !m.sort) {
        error = "the translucent pipelines do not build";
        return false;
    }
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                 width:m.width
                                                                                height:m.height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    m.depth = [m.device newTextureWithDescriptor:d];
    MTLTextureDescriptor* w = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:1
                                                                                height:1
                                                                             mipmapped:NO];
    m.white = [m.device newTextureWithDescriptor:w];
    const uint32_t white = 0xFFFFFFFFu;
    [m.white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
    m.heads = [m.device newBufferWithLength:static_cast<NSUInteger>(m.width) * m.height * 4
                                    options:MTLResourceStorageModePrivate];
    m.counters = [m.device newBufferWithLength:kMaxBands * 4 options:MTLResourceStorageModeShared];
    m.slices = [m.device newBufferWithLength:(m.height / kSliceRows) * 4 options:MTLResourceStorageModeShared];
    std::memset(m.slices.contents, 0, m.slices.length);
    m.lists(m.width * m.height * 4);
    m.bands = {{0, m.height, 0}};
    if (window) {
        m.window = window;
        m.view = SDL_Metal_CreateView(window);
        m.layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m.view);
        m.layer.device = m.device;
        m.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        // The run paces itself by the guest's clock, so presenting must not wait for the display.
        m.layer.displaySyncEnabled = NO;
        m.present = m.make(@"present_vertex", @"present_fragment", MTLPixelFormatBGRA8Unorm, false, nil);
        m.overlay = m.make(@"present_vertex", @"present_fragment", MTLPixelFormatBGRA8Unorm, false,
                           ^(MTLRenderPipelineColorAttachmentDescriptor* c) {
                               c.blendingEnabled = YES;
                               c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorOne;
                               c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor =
                                   MTLBlendFactorOneMinusSourceAlpha;
                           });
        MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
        s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear;
        m.present_sampler = [m.device newSamplerStateWithDescriptor:s];
    }
    return true;
}

const char* Gpu::driver() const { return impl_->device ? impl_->device.name.UTF8String : "none"; }

void Gpu::last_render(double& gpu_ms, uint32_t& fragments) {
    impl_->finish();
    gpu_ms = impl_->last ? (impl_->last.GPUEndTime - impl_->last.GPUStartTime) * 1000.0 : 0.0;
    fragments = 0;
    if (impl_->counters)
        for (size_t b = 0; b < impl_->bands.size(); ++b)
            fragments += static_cast<const uint32_t*>(impl_->counters.contents)[b];
}

void Gpu::render(const DisplayList& list, const Hardware& hw) { render(translate(list, hw), hw); }

void Gpu::render(const Frame& frame, const Hardware& hw) {
    Impl& m = *impl_;
    if (!m.device)
        return;
    // The CPU writes textures and the vertex buffer that the last render reads.
    m.finish();
    const uint64_t render = renders + 1;

    const float background_depth = frame.background_depth;
    const uint32_t argb = frame.background;
    const MTLClearColor background = MTLClearColorMake(((argb >> 16) & 255) / 255.0, ((argb >> 8) & 255) / 255.0,
                                                       (argb & 255) / 255.0, (argb >> 24) / 255.0);
    float z_scale = std::max(frame.max_z, background_depth);
    if (!(z_scale > 0.0f))
        z_scale = 1.0f;

    m.draws.clear();
    m.triangles.clear();
    for (List which : {List::Opaque, List::PunchThrough, List::Translucent}) {
        uint32_t index = 0;  // the draw's place in its list
        for (const render::Draw& s : frame.draws) {
            if (s.list != which)
                continue;
            const uint32_t order = index++;
            if (s.depth == DepthTest::Never)
                continue;
            Batch d{};
            d.first = static_cast<uint32_t>(m.triangles.size());
            for (uint32_t i = 0; i + 2 < s.count; ++i) {
                Vertex t[3] = {frame.vertices[s.first + i], frame.vertices[s.first + i + 1],
                               frame.vertices[s.first + i + 2]};
                // A flat strip takes each triangle's colours from its last vertex.
                if (!s.gouraud) {
                    t[0].base = t[1].base = t[2].base;
                    t[0].offset = t[1].offset = t[2].offset;
                }
                m.triangles.insert(m.triangles.end(), t, t + 3);
            }
            d.count = static_cast<uint32_t>(m.triangles.size()) - d.first;
            d.translucent = which == List::Translucent;
            d.pipeline = static_cast<uint32_t>(s.source) | static_cast<uint32_t>(s.target) << 3;
            // A translucent polygon tests depth but does not write it, whatever its ISP word says: the
            // hardware blends each pixel's translucent layers in depth order, so none hides another.
            const bool write = s.depth_write && !d.translucent;
            d.depth = static_cast<uint32_t>(s.depth) | write << 3;
            d.texture = s.textured ? m.texture(s, frame, hw, render, textures_decoded, untextured) : m.white;
            const uint32_t filter = static_cast<uint32_t>(s.filter);
            const bool mipmapped = s.textured && s.texture.mipmapped;
            d.sampler = (filter != 0) | mipmapped << 1 | (filter >= 2) << 2 | s.clamp_u << 3 | s.clamp_v << 4 |
                        s.flip_u << 5 | s.flip_v << 6;
            int x0 = frame.clip_x0, y0 = frame.clip_y0, x1 = frame.clip_x1, y1 = frame.clip_y1;
            if (s.clip == Clip::Inside) {
                x0 = std::max(x0, static_cast<int>(s.clip_x0));
                y0 = std::max(y0, static_cast<int>(s.clip_y0));
                x1 = std::min(x1, static_cast<int>(s.clip_x1));
                y1 = std::min(y1, static_cast<int>(s.clip_y1));
            }
            x0 = std::clamp(x0, 0, 640);
            x1 = std::clamp(x1, x0, 640);
            y0 = std::clamp(y0, 0, 480);
            y1 = std::clamp(y1, y0, 480);
            const auto sc = static_cast<NSUInteger>(m.scale);
            d.scissor = {static_cast<NSUInteger>(x0) * sc, static_cast<NSUInteger>(y0) * sc,
                         static_cast<NSUInteger>(x1 - x0) * sc, static_cast<NSUInteger>(y1 - y0) * sc};
            d.uniforms.flags = s.textured | s.offset_colour << 1 | s.use_alpha << 2 | s.ignore_texture_alpha << 3 |
                               (which == List::PunchThrough) << 4;
            d.uniforms.shading = static_cast<int32_t>(s.shading);
            d.uniforms.fog = static_cast<int32_t>(s.fog);
            d.uniforms.blend = d.pipeline;
            d.uniforms.alpha_ref = frame.alpha_ref;
            d.uniforms.order = order;
            d.uniforms.lod_bias = s.mipmap_bias;
            if (d.count && d.scissor.width > 0 && d.scissor.height > 0)
                m.draws.push_back(d);
        }
    }

    const size_t vertex_bytes = m.triangles.size() * sizeof(Vertex);
    if (vertex_bytes > (m.vertices ? m.vertices.length : 0))
        m.vertices = [m.device newBufferWithLength:std::max<size_t>(vertex_bytes * 2, 1 << 20)
                                           options:MTLResourceStorageModeShared];
    if (vertex_bytes)
        std::memcpy(m.vertices.contents, m.triangles.data(), vertex_bytes);

    Impl::Pending& p = m.pending;
    p = {};
    p.open = true;
    p.target = frame.target;
    p.background = background;
    p.background_depth = background_depth;
    p.z_scale = z_scale;
    colour_of(frame.fog_table_colour, p.fog.table_colour);
    colour_of(frame.fog_vertex_colour, p.fog.vertex_colour);
    p.fog.params[0] = frame.fog_density;
    p.fog.params[1] = z_scale;
    for (int i = 0; i < 128; ++i) {
        p.fog.table[i / 2][(i & 1) * 2] = static_cast<float>(frame.fog_table[i][0]) / 255.0f;
        p.fog.table[i / 2][(i & 1) * 2 + 1] = static_cast<float>(frame.fog_table[i][1]) / 255.0f;
    }
    m.encode();
    ++renders;
    if (renders % 600 == 0) {
        // Textures no render has used for a minute.
        for (auto it = m.textures.begin(); it != m.textures.end();)
            it = renders > 3600 && it->second.used + 3600 < renders ? m.textures.erase(it) : std::next(it);
    }
}

void Gpu::present(const Hardware& hw) {
    Impl& m = *impl_;
    if (!m.layer)
        return;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(m.window, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return;
    m.layer.drawableSize = CGSizeMake(pw, ph);
    // The display shows the render that the GPU may still draw only once it is checked.
    if (m.pending.open && m.pending.target == (hw.registers[kFbRSof1] & 0xFFFFFF))
        m.finish();
    id<CAMetalDrawable> drawable = [m.layer nextDrawable];
    if (!drawable)
        return;
    id<MTLCommandBuffer> cmd = [m.queue commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    auto it = m.targets.find(hw.registers[kFbRSof1] & 0xFFFFFF);
    // The display shows only what a render drew. A title that writes a frame into video memory
    // itself would show black: IKARUGA_RENDER_DEBUG reports each such vertical blank.
    if (it == m.targets.end() && renders > 0 && std::getenv("IKARUGA_RENDER_DEBUG"))
        std::fprintf(stderr, "render: the display shows %08x, which no render drew\n", hw.registers[kFbRSof1]);
    if (it != m.targets.end()) {
        // 4:3 in the middle of the window, with black bars where it is wider or taller.
        double w = pw, h = pw * 3.0 / 4.0;
        if (h > ph) {
            h = ph;
            w = ph * 4.0 / 3.0;
        }
        [enc setViewport:(MTLViewport){(pw - w) / 2, (ph - h) / 2, w, h, 0, 1}];
        [enc setRenderPipelineState:m.present];
        [enc setFragmentTexture:it->second atIndex:0];
        [enc setFragmentSamplerState:m.present_sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    if (m.overlay_shown && m.overlay_shown.width == static_cast<NSUInteger>(pw) &&
        m.overlay_shown.height == static_cast<NSUInteger>(ph)) {
        [enc setViewport:(MTLViewport){0, 0, static_cast<double>(pw), static_cast<double>(ph), 0, 1}];
        [enc setRenderPipelineState:m.overlay];
        [enc setFragmentTexture:m.overlay_shown atIndex:0];
        [enc setFragmentSamplerState:m.present_sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];
    [cmd presentDrawable:drawable];
    [cmd commit];
}

void Gpu::set_overlay(const uint32_t* pixels, uint32_t width, uint32_t height) {
    Impl& m = *impl_;
    if (!pixels || !width || !height || !m.device) {
        m.overlay_shown = nil;
        return;
    }
    __strong id<MTLTexture>& t = m.overlays[m.overlay_next];
    m.overlay_next ^= 1;
    if (!t || t.width != width || t.height != height) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        t = [m.device newTextureWithDescriptor:d];
    }
    [t replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:pixels bytesPerRow:width * 4];
    m.overlay_shown = t;
}

bool Gpu::read_displayed(const Hardware& hw, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    return read_target(hw.registers[kFbRSof1] & 0xFFFFFF, pixels, width, height);
}

bool Gpu::read_target(uint32_t address, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    auto it = impl_->targets.find(address);
    return it != impl_->targets.end() && impl_->download(it->second, 0, pixels, width, height);
}

bool Gpu::read_texture(uint32_t tcw, uint32_t tsp, uint32_t palette_format, std::vector<uint32_t>& pixels,
                       uint32_t& width, uint32_t& height) {
    TextureFormat f;
    if (!describe_texture(tcw, tsp, 0, f))
        return false;
    auto it = impl_->textures.find(texture_key(f, palette_format & 3));
    return it != impl_->textures.end() && impl_->download(it->second.texture, 0, pixels, width, height);
}

}  // namespace render
