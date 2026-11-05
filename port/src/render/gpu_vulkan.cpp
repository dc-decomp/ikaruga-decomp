// The renderer in Vulkan, the port of the Metal one (gpu_metal.mm) to Linux and Windows. See gpu.h
// and shaders/*.glsl for the drawing itself. Each render draws into the target of the framebuffer
// that FB_W_SOF1 names; the window shows the newest render at the next vertical blank, as the
// console shows the picture it scanned out a frame after the PowerVR drew it. The translucent
// polygons of each pixel blend from the farthest to the nearest, as the PowerVR's autosort mode
// does, through a per-pixel list in a storage buffer and a full-screen sort pass.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/english_textures.h"
#include "render/gpu.h"
#include "render/texture.h"

// The SPIR-V arrays that the build embeds (port/cmake/wrap_spirv.cmake).
#include "geometry_vert.h"
#include "geometry_frag.h"
#include "translucent_frag.h"
#include "added_frag.h"
#include "added_top_frag.h"
#include "sort_frag.h"
#include "present_vert.h"
#include "present_frag.h"
#include "bloom_extract_frag.h"
#include "bloom_blur_frag.h"
#include "bloom_composite_frag.h"

namespace render {
namespace {

constexpr size_t kFbRSof1 = 0x050 / 4;

// Defined below, beside the swapchain code.
VkRenderPass make_swap_pass(VkDevice device, VkFormat format);

// The shaders' structures (shaders/*.glsl).
struct Screen {
    float scale[4];
};
struct DrawUniforms {
    int32_t flags = 0, shading = 0, fog = 0;
    uint32_t blend = 0;
    float alpha_ref = 0;
    uint32_t order = 0;
    float lod_bias = 0, pad = 0;
    uint32_t width = 0, capacity = 0, band = 0;
};
struct PushConstants {
    Screen screen{};
    DrawUniforms draw{};
};
struct FogUniforms {
    float table_colour[4]{};
    float vertex_colour[4]{};
    float params[4]{};
    float table[64][4]{};
};
struct Pass {
    float step[2]{};
    int32_t taps = 1;
    float strength = 0;
};
struct SortUniforms {
    uint32_t width = 0, capacity = 0;
};

constexpr uint32_t kGlowWidth = 320, kGlowHeight = 240;
constexpr float kBloomStrength[3] = {0.0f, 0.3f, 0.6f};
constexpr size_t kNodeBytes = 16;
constexpr uint32_t kSliceRows = 16;
constexpr uint32_t kMaxBands = 256;
constexpr uint32_t kMaxPoolNodes = 2560 * 1920 * 4;

struct Band {
    uint32_t y0, y1;
    uint64_t estimate;
};

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

uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h = 0) {
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

uint64_t texture_key(const TextureFormat& t, uint32_t palette_format) {
    const uint32_t fields[] = {t.address, t.width, t.height, static_cast<uint32_t>(t.pixel), t.twiddled, t.vq,
                               t.mipmapped, t.row_texels, t.palette_base, palette_format};
    return hash_bytes(reinterpret_cast<const uint8_t*>(fields), sizeof fields, 0);
}

// Metal's framebuffer has its origin at the top left and Metal's clip space has y up, so the
// vertex shaders' y = +1 is row 0. Vulkan's clip space has y down. A viewport with a negative
// height flips it (core since Vulkan 1.1), so every pass draws, samples and scissors the rows
// as Metal's do: the targets are top-down, as read_target() and the screenshots expect.
VkViewport metal_viewport(float x, float y, float w, float h) { return VkViewport{x, y + h, w, -h, 0.0f, 1.0f}; }

VkBlendFactor blend_factor(uint32_t instruction, bool source) {
    switch (instruction & 7) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 2: return source ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
        case 3: return source ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_DST_ALPHA;
        default: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

VkCompareOp compare(uint32_t mode) {
    constexpr VkCompareOp ops[8] = {VK_COMPARE_OP_NEVER,           VK_COMPARE_OP_LESS,
                                    VK_COMPARE_OP_EQUAL,           VK_COMPARE_OP_LESS_OR_EQUAL,
                                    VK_COMPARE_OP_GREATER,         VK_COMPARE_OP_NOT_EQUAL,
                                    VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return ops[mode & 7];
}

void colour_of(uint32_t reg, float (&out)[4]) {
    out[0] = static_cast<float>((reg >> 16) & 255) / 255.0f;
    out[1] = static_cast<float>((reg >> 8) & 255) / 255.0f;
    out[2] = static_cast<float>(reg & 255) / 255.0f;
    out[3] = 1.0f;
}

struct CachedTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t hash = 0, used = 0;
    uint32_t levels = 1, width = 1, height = 1;
};

struct Batch {
    uint32_t first = 0, count = 0;
    bool translucent = false;
    bool adds = false;
    uint32_t pipeline = 0;
    uint32_t depth = 0;
    uint32_t sampler = 0;
    int32_t scissor[4] = {0, 0, 0, 0};
    DrawUniforms uniforms;
    CachedTexture* texture = nullptr;
};

const char* vk_result(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "success";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
        case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
        case VK_ERROR_DEVICE_LOST: return "device lost";
        case VK_ERROR_MEMORY_MAP_FAILED: return "memory map failed";
        case VK_ERROR_LAYER_NOT_PRESENT: return "layer not present";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "format not supported";
        case VK_ERROR_SURFACE_LOST_KHR: return "surface lost";
        case VK_ERROR_OUT_OF_DATE_KHR: return "out of date";
        default: return "Vulkan error";
    }
}

}  // namespace

struct Gpu::Impl {
    SDL_Window* window = nullptr;
    unsigned scale = 1;
    uint32_t width = 640, height = 480;
    unsigned bloom_level = 0, anisotropy = 1;
    bool english = false, display_sync = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphics_family = 0, present_family = 0;
    VkQueue graphics_queue = VK_NULL_HANDLE, present_queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool fence_pending = false;
    VkSemaphore image_available = VK_NULL_HANDLE, render_finished = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties properties{};
    bool sampler_anisotropy = false;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    std::vector<VkFramebuffer> swap_framebuffers;
    VkRenderPass swap_pass = VK_NULL_HANDLE;
    VkFormat swap_pass_format = VK_FORMAT_UNDEFINED;

    VkRenderPass band_clear_pass = VK_NULL_HANDLE, band_load_pass = VK_NULL_HANDLE;
    VkRenderPass added_pass = VK_NULL_HANDLE, sort_pass = VK_NULL_HANDLE, top_pass = VK_NULL_HANDLE;
    VkRenderPass glow_pass = VK_NULL_HANDLE, shown_pass = VK_NULL_HANDLE;

    VkDescriptorSetLayout geometry_layout = VK_NULL_HANDLE, sort_layout = VK_NULL_HANDLE,
                            present_layout = VK_NULL_HANDLE, composite_layout = VK_NULL_HANDLE;
    VkPipelineLayout geometry_pipeline_layout = VK_NULL_HANDLE, sort_pipeline_layout = VK_NULL_HANDLE,
                     present_pipeline_layout = VK_NULL_HANDLE, composite_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

    VkShaderModule vs_geometry = VK_NULL_HANDLE, fs_geometry = VK_NULL_HANDLE, fs_translucent = VK_NULL_HANDLE,
                   fs_added = VK_NULL_HANDLE, fs_added_top = VK_NULL_HANDLE, fs_sort = VK_NULL_HANDLE,
                   vs_present = VK_NULL_HANDLE, fs_present = VK_NULL_HANDLE, fs_bloom_extract = VK_NULL_HANDLE,
                   fs_bloom_blur = VK_NULL_HANDLE, fs_bloom_composite = VK_NULL_HANDLE;

    std::map<uint32_t, VkPipeline> pipelines;  // opaque: blend | depth << 6 | write << 9
    std::map<uint32_t, VkPipeline> translucent_pipelines, added_pipelines, added_top_pipelines;
    VkPipeline sort = VK_NULL_HANDLE, present = VK_NULL_HANDLE, overlay = VK_NULL_HANDLE,
               bloom_extract = VK_NULL_HANDLE, bloom_half = VK_NULL_HANDLE, bloom_blur = VK_NULL_HANDLE,
               bloom_composite = VK_NULL_HANDLE;
    std::map<uint32_t, VkSampler> samplers;
    VkSampler present_sampler = VK_NULL_HANDLE;

    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImage depth = VK_NULL_HANDLE;
        VkDeviceMemory depth_memory = VK_NULL_HANDLE;
        VkImageView depth_view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint64_t used = 0;
    };
    std::map<uint32_t, Target> targets;
    CachedTexture white_texture;

    VkImage scratch = VK_NULL_HANDLE, shown = VK_NULL_HANDLE;
    VkDeviceMemory scratch_memory = VK_NULL_HANDLE, shown_memory = VK_NULL_HANDLE;
    VkImageView scratch_view = VK_NULL_HANDLE, shown_view = VK_NULL_HANDLE;
    VkImageLayout scratch_layout = VK_IMAGE_LAYOUT_UNDEFINED, shown_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFramebuffer shown_framebuffer = VK_NULL_HANDLE;

    VkImage glow[4] = {};
    VkDeviceMemory glow_memory[4] = {};
    VkImageView glow_view[4] = {};
    VkFramebuffer glow_framebuffer[4] = {};
    VkImageLayout glow_layout[4] = {};

    VkBuffer heads = VK_NULL_HANDLE, nodes = VK_NULL_HANDLE, counters = VK_NULL_HANDLE, slices = VK_NULL_HANDLE,
             farthest = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
    VkDeviceMemory heads_memory = VK_NULL_HANDLE, nodes_memory = VK_NULL_HANDLE, counters_memory = VK_NULL_HANDLE,
                    slices_memory = VK_NULL_HANDLE, farthest_memory = VK_NULL_HANDLE, nearest_memory = VK_NULL_HANDLE;
    uint32_t node_capacity = 0;
    uint32_t* counters_map = nullptr;
    uint32_t* slices_map = nullptr;
    std::vector<Band> bands;

    VkBuffer vertices = VK_NULL_HANDLE;
    VkDeviceMemory vertices_memory = VK_NULL_HANDLE;
    size_t vertices_size = 0;
    Vertex* vertex_map = nullptr;
    VkBuffer fog_buffer = VK_NULL_HANDLE;
    VkDeviceMemory fog_memory = VK_NULL_HANDLE;
    FogUniforms* fog_map = nullptr;

    std::unordered_map<uint64_t, CachedTexture> textures;

    struct Overlay {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t width = 0, height = 0;
    } overlays[2];
    int overlay_next = 0;
    Overlay* overlay_shown = nullptr;

    std::vector<Vertex> triangles;
    std::vector<Batch> draws;
    std::vector<uint32_t> texels;
    uint32_t newest = 0;
    bool newest_valid = false;
    uint64_t renders_total = 0, encodes = 0;
    double frame_start = 0, work = 0;
    double presented_time = 0;
    double last_gpu_ms = 0;
    uint64_t late_presents = 0;
    bool hud = false;
    float background[4] = {0, 0, 0, 1};
    float z_scale = 1;

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        return 0;
    }

    void buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, VkBuffer& out,
                VkDeviceMemory& memory) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &info, nullptr, &out) != VK_SUCCESS)
            return;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, out, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, flags);
        if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS)
            return;
        vkBindBufferMemory(device, out, memory, 0);
    }

    void image(uint32_t w, uint32_t h, uint32_t levels, VkFormat format, VkImageUsageFlags usage, VkImage& out,
               VkDeviceMemory& memory) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent = {w, h, 1};
        info.mipLevels = levels;
        info.arrayLayers = 1;
        info.format = format;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = usage;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &info, nullptr, &out) != VK_SUCCESS)
            return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, out, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS)
            return;
        vkBindImageMemory(device, out, memory, 0);
    }

    VkImageView view(VkImage image, VkFormat format, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = {aspect, 0, 1, 0, 1};
        VkImageView out = VK_NULL_HANDLE;
        vkCreateImageView(device, &info, nullptr, &out);
        return out;
    }

    VkRenderPass render_pass(VkFormat colour, VkImageLayout final_layout, VkAttachmentLoadOp colour_load, bool depth,
                             VkAttachmentLoadOp depth_load) {
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = colour;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = colour_load;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[0].finalLayout = final_layout;
        VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colour_ref;
        subpass.pDepthStencilAttachment = depth ? &depth_ref : nullptr;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        if (depth) {
            attachments[1].format = VK_FORMAT_D32_SFLOAT;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = depth_load;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = depth ? 2 : 1;
        info.pAttachments = attachments;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 2;
        info.pDependencies = deps;
        VkRenderPass out = VK_NULL_HANDLE;
        vkCreateRenderPass(device, &info, nullptr, &out);
        return out;
    }

    void transition(VkCommandBuffer buffer, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        if (from == to || image == VK_NULL_HANDLE)
            return;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        // Every mip level: a texture's lower levels stay UNDEFINED otherwise, and sampling them is invalid.
        barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }

    VkShaderModule module(const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size;
        info.pCode = code;
        VkShaderModule out = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &info, nullptr, &out) != VK_SUCCESS)
            std::fprintf(stderr, "render: a shader module does not build\n");
        return out;
    }

    VkDescriptorSet descriptor(VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = descriptor_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet out = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &info, &out) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return out;
    }

    void write_buffer(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer buffer,
                      VkDeviceSize size) {
        VkDescriptorBufferInfo info{buffer, 0, size};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void write_image(VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler sampler) {
        VkDescriptorImageInfo info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    VkPipeline build(VkShaderModule vertex, VkShaderModule fragment, VkRenderPass pass, VkPipelineLayout layout,
                     bool vertex_input, bool depth, bool blend, VkBlendFactor source, VkBlendFactor target,
                     VkCompareOp depth_compare, bool depth_write, bool colour_write) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[4] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, x))},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, u))},
            {2, 0, VK_FORMAT_R8G8B8A8_UINT, static_cast<uint32_t>(offsetof(Vertex, base))},
            {3, 0, VK_FORMAT_R8G8B8A8_UINT, static_cast<uint32_t>(offsetof(Vertex, offset))},
        };
        VkPipelineVertexInputStateCreateInfo vertex_info{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (vertex_input) {
            vertex_info.vertexBindingDescriptionCount = 1;
            vertex_info.pVertexBindingDescriptions = &binding;
            vertex_info.vertexAttributeDescriptionCount = 4;
            vertex_info.pVertexAttributeDescriptions = attributes;
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth_state.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
        depth_state.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
        depth_state.depthCompareOp = depth ? depth_compare : VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState colour_state{};
        colour_state.blendEnable = blend ? VK_TRUE : VK_FALSE;
        colour_state.srcColorBlendFactor = source;
        colour_state.dstColorBlendFactor = target;
        colour_state.srcAlphaBlendFactor = source;
        colour_state.dstAlphaBlendFactor = target;
        colour_state.colorWriteMask =
            colour_write ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT)
                         : 0;
        VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend_state.attachmentCount = 1;
        blend_state.pAttachments = &colour_state;
        const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates = dynamic;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_info;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth_state;
        info.pColorBlendState = &blend_state;
        info.pDynamicState = &dynamic_state;
        info.layout = layout;
        info.renderPass = pass;
        info.subpass = 0;
        VkPipeline out = VK_NULL_HANDLE;
        const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &out);
        if (r != VK_SUCCESS)
            std::fprintf(stderr, "render: a pipeline does not build: %s\n", vk_result(r));
        return out;
    }

    VkPipeline pipeline(uint32_t key) {
        if (auto it = pipelines.find(key); it != pipelines.end())
            return it->second;
        const uint32_t src = key & 7, dst = (key >> 3) & 7;
        const bool write = (key >> 9) & 1;
        VkPipeline p = build(vs_geometry, fs_geometry, band_load_pass, geometry_pipeline_layout, true, true,
                             !(src == 1 && dst == 0), blend_factor(src, true), blend_factor(dst, false),
                             compare(key >> 6), write, true);
        pipelines[key] = p;
        return p;
    }

    VkPipeline list_pipeline(std::map<uint32_t, VkPipeline>& cache, VkShaderModule fragment, uint32_t depth,
                             uint32_t key) {
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
        VkPipeline p = build(vs_geometry, fragment, band_load_pass, geometry_pipeline_layout, true, true, false,
                             VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, compare(depth), false,
                             fragment != fs_translucent);
        cache[key] = p;
        return p;
    }

    VkSampler sampler(uint32_t key) {
        if (auto it = samplers.find(key); it != samplers.end())
            return it->second;
        const bool bilinear = key & 1, mipmapped = key & 2, trilinear = key & 4;
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.minFilter = info.magFilter;
        info.mipmapMode = !mipmapped ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                     : trilinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = (key & 8) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                      : (key & 32) ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                   : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = (key & 16) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                       : (key & 64) ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                    : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.anisotropyEnable = sampler_anisotropy && bilinear && anisotropy > 1;
        info.maxAnisotropy = static_cast<float>(std::clamp(anisotropy, 1u, 16u));
        info.maxLod = mipmapped ? VK_LOD_CLAMP_NONE : 0.0f;
        info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        VkSampler out = VK_NULL_HANDLE;
        if (vkCreateSampler(device, &info, nullptr, &out) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        samplers[key] = out;
        return out;
    }

    VkCommandBuffer begin_once() {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = command_pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer one = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &info, &one);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(one, &begin);
        return one;
    }

    void end_once(VkCommandBuffer one) {
        vkEndCommandBuffer(one);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &one;
        vkQueueSubmit(graphics_queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphics_queue);
        vkFreeCommandBuffers(device, command_pool, 1, &one);
    }

    void wait() {
        // The fence is signaled only by a render submission; presentation consumes and resets it
        // without submitting a fence of its own. Waiting when nothing is pending would either block
        // the first frame or hang after a present.
        if (!device || !fence || !fence_pending)
            return;
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);
        fence_pending = false;
    }

    void upload_image(CachedTexture& t, uint32_t level, uint32_t w, uint32_t h, const std::vector<uint32_t>& pixels) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, staging_memory);
        if (!staging)
            return;
        void* mapped = nullptr;
        vkMapMemory(device, staging_memory, 0, bytes, 0, &mapped);
        std::memcpy(mapped, pixels.data(), static_cast<size_t>(bytes));
        vkUnmapMemory(device, staging_memory);
        VkCommandBuffer one = begin_once();
        transition(one, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(one, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(one, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        end_once(one);
        t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, staging_memory, nullptr);
    }

    CachedTexture& texture(const render::Draw& d, const Frame& frame, const Hardware& hw, uint64_t render,
                           uint64_t& decoded, uint64_t& untextured) {
        const TextureFormat& f = d.texture;
        const uint32_t* palette = frame.palette;
        auto without = [&](const char* why) -> CachedTexture& {
            static std::vector<uint32_t> shown;
            ++untextured;
            if (std::getenv("IKARUGA_RENDER_DEBUG") &&
                std::find(shown.begin(), shown.end(), f.address) == shown.end()) {
                shown.push_back(f.address);
                std::fprintf(stderr, "render: the texture at %06x drawn without itself: %s\n", f.address, why);
            }
            return white_texture;
        };
        if (!d.texture_readable)
            return without("an encoding the decoder does not read");
        CachedTexture& t = textures[texture_key(f, frame.palette_format)];
        if (t.image && t.used == render)
            return t;
        uint32_t first, size;
        texture_extent(f, first, size);
        if (static_cast<size_t>(first) + size > hw.vram_size)
            return without("outside video memory");
        uint64_t h = hash_bytes(hw.vram + first, size, 0);
        if (f.pixel == PixelFormat::Palette4 || f.pixel == PixelFormat::Palette8) {
            const uint32_t entries = f.pixel == PixelFormat::Palette4 ? 16 : 256;
            h = hash_bytes(reinterpret_cast<const uint8_t*>(palette + f.palette_base), entries * 4, h);
        }
        const bool english = this->english && has_english(h);
        const uint64_t content = h;
        if (english)
            h = ~h;
        t.used = render;
        if (t.image && t.hash == h)
            return t;
        if (!t.image) {
            t.width = f.width;
            t.height = f.height;
            t.levels = f.levels();
            image(t.width, t.height, t.levels, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t.image, t.memory);
            t.view = view(t.image, VK_FORMAT_R8G8B8A8_UNORM);
            if (!t.image)
                return without("Vulkan has no room for it");
        }
        t.hash = h;
        ++decoded;
        for (uint32_t level = 0; level < f.levels(); ++level) {
            if (!decode_texture(f, level, hw.vram, hw.vram_size, palette, texels))
                break;
            const uint32_t w = std::max(1u, f.width >> level), hh = std::max(1u, f.height >> level);
            static const char* dump = std::getenv("IKARUGA_DUMP_TEXTURES");
            static std::unordered_map<uint64_t, bool> dumped;
            if (dump && level == 0 && !dumped[content]) {
                dumped[content] = true;
                char path[512];
                std::snprintf(path, sizeof path, "%s/%016llx-%ux%u.rgba", dump,
                              static_cast<unsigned long long>(content), w, hh);
                if (std::FILE* out = std::fopen(path, "wb")) {
                    std::fwrite(texels.data(), 4, texels.size(), out);
                    std::fclose(out);
                }
            }
            if (english && level == 0)
                write_english(content, w, hh, texels);
            upload_image(t, level, w, hh, texels);
        }
        return t;
    }

    void destroy_targets();
    void size_buffers();
    void encode();
    void create_swapchain();
    void destroy_swapchain();
    void fullscreen(VkCommandBuffer buffer, VkRenderPass pass, VkFramebuffer framebuffer, VkExtent2D extent,
                    VkPipeline pipeline_state, VkPipelineLayout layout, VkDescriptorSetLayout set_layout,
                    const VkImageView* images, uint32_t image_count, const Pass& constants, VkImage* target_image,
                    VkImageLayout* target_layout, VkImageLayout final_layout);
    void bloom(VkCommandBuffer buffer);
    void make_white();
    bool read_image(VkImage image, uint32_t iw, uint32_t ih, VkImageLayout from, VkImageLayout restore,
                    std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h);
};

bool Gpu::Impl::read_image(VkImage image, uint32_t iw, uint32_t ih, VkImageLayout from, VkImageLayout restore,
                           std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h) {
    if (!device || !image)
        return false;
    wait();
    w = iw;
    h = ih;
    pixels.resize(static_cast<size_t>(w) * h);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, staging_memory);
    if (!staging)
        return false;
    VkCommandBuffer one = begin_once();
    transition(one, image, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(one, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
    transition(one, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, restore);
    end_once(one);
    void* mapped = nullptr;
    if (vkMapMemory(device, staging_memory, 0, bytes, 0, &mapped) == VK_SUCCESS) {
        std::memcpy(pixels.data(), mapped, static_cast<size_t>(bytes));
        vkUnmapMemory(device, staging_memory);
    }
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);
    return true;
}

void Gpu::Impl::destroy_targets() {
    for (auto& [address, t] : targets) {
        vkDestroyFramebuffer(device, t.framebuffer, nullptr);
        vkDestroyImageView(device, t.view, nullptr);
        vkDestroyImage(device, t.image, nullptr);
        vkFreeMemory(device, t.memory, nullptr);
        vkDestroyImageView(device, t.depth_view, nullptr);
        vkDestroyImage(device, t.depth, nullptr);
        vkFreeMemory(device, t.depth_memory, nullptr);
    }
    targets.clear();
    for (int i = 0; i < 4; ++i) {
        if (glow_framebuffer[i]) vkDestroyFramebuffer(device, glow_framebuffer[i], nullptr);
        if (glow_view[i]) vkDestroyImageView(device, glow_view[i], nullptr);
        if (glow[i]) vkDestroyImage(device, glow[i], nullptr);
        if (glow_memory[i]) vkFreeMemory(device, glow_memory[i], nullptr);
        glow_framebuffer[i] = VK_NULL_HANDLE;
        glow_view[i] = VK_NULL_HANDLE;
        glow[i] = VK_NULL_HANDLE;
        glow_memory[i] = VK_NULL_HANDLE;
        glow_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (shown_framebuffer) vkDestroyFramebuffer(device, shown_framebuffer, nullptr);
    if (shown_view) vkDestroyImageView(device, shown_view, nullptr);
    if (shown) vkDestroyImage(device, shown, nullptr);
    if (shown_memory) vkFreeMemory(device, shown_memory, nullptr);
    if (scratch_view) vkDestroyImageView(device, scratch_view, nullptr);
    if (scratch) vkDestroyImage(device, scratch, nullptr);
    if (scratch_memory) vkFreeMemory(device, scratch_memory, nullptr);
    shown_framebuffer = VK_NULL_HANDLE;
    shown_view = VK_NULL_HANDLE;
    shown = VK_NULL_HANDLE;
    shown_memory = VK_NULL_HANDLE;
    scratch_view = VK_NULL_HANDLE;
    scratch = VK_NULL_HANDLE;
    scratch_memory = VK_NULL_HANDLE;
    scratch_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    shown_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void Gpu::Impl::size_buffers() {
    destroy_targets();
    VkBuffer* buffers[] = {&heads, &farthest, &nearest};
    VkDeviceMemory* memories[] = {&heads_memory, &farthest_memory, &nearest_memory};
    for (int i = 0; i < 3; ++i) {
        if (*buffers[i]) vkDestroyBuffer(device, *buffers[i], nullptr);
        if (*memories[i]) vkFreeMemory(device, *memories[i], nullptr);
        buffer(static_cast<VkDeviceSize>(width) * height * 4,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, *buffers[i], *memories[i]);
    }
    if (counters) vkDestroyBuffer(device, counters, nullptr);
    if (counters_memory) vkFreeMemory(device, counters_memory, nullptr);
    if (slices) vkDestroyBuffer(device, slices, nullptr);
    if (slices_memory) vkFreeMemory(device, slices_memory, nullptr);
    buffer(kMaxBands * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, counters, counters_memory);
    buffer(static_cast<VkDeviceSize>(height / kSliceRows) * 4,
           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, slices, slices_memory);
    vkMapMemory(device, counters_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&counters_map));
    vkMapMemory(device, slices_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&slices_map));
    std::memset(counters_map, 0, kMaxBands * 4);
    std::memset(slices_map, 0, static_cast<size_t>(height / kSliceRows) * 4);
    // The sort cannot read and write one attachment in a pass, so the target's picture is copied
    // aside for the sort pass to read.
    image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          scratch, scratch_memory);
    scratch_view = view(scratch, VK_FORMAT_R8G8B8A8_UNORM);
    scratch_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          shown, shown_memory);
    shown_view = view(shown, VK_FORMAT_R8G8B8A8_UNORM);
    shown_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = shown_pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &shown_view;
    fb.width = width;
    fb.height = height;
    fb.layers = 1;
    vkCreateFramebuffer(device, &fb, nullptr, &shown_framebuffer);
    for (int i = 0; i < 4; ++i) {
        const uint32_t gw = kGlowWidth >> (i / 2), gh = kGlowHeight >> (i / 2);
        image(gw, gh, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, glow[i], glow_memory[i]);
        glow_view[i] = view(glow[i], VK_FORMAT_R16G16B16A16_SFLOAT);
        glow_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        fb.renderPass = glow_pass;
        fb.pAttachments = &glow_view[i];
        fb.width = gw;
        fb.height = gh;
        vkCreateFramebuffer(device, &fb, nullptr, &glow_framebuffer[i]);
    }
    node_capacity = std::min(width * height * 4, kMaxPoolNodes);
    // The descriptor range and the shader's capacity follow node_capacity, so the buffer has to be
    // reallocated when a scale change makes it larger or smaller.
    if (nodes) vkDestroyBuffer(device, nodes, nullptr);
    if (nodes_memory) vkFreeMemory(device, nodes_memory, nullptr);
    nodes = VK_NULL_HANDLE;
    nodes_memory = VK_NULL_HANDLE;
    buffer(static_cast<VkDeviceSize>(node_capacity) * kNodeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nodes, nodes_memory);
    bands = {{0, height, 0}};
}

void Gpu::Impl::make_white() {
    white_texture.width = white_texture.height = 1;
    white_texture.levels = 1;
    image(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          white_texture.image, white_texture.memory);
    white_texture.view = view(white_texture.image, VK_FORMAT_R8G8B8A8_UNORM);
    const std::vector<uint32_t> pixel = {0xFFFFFFFFu};
    upload_image(white_texture, 0, 1, 1, pixel);
}

void Gpu::Impl::fullscreen(VkCommandBuffer buffer, VkRenderPass pass, VkFramebuffer framebuffer,
                           VkExtent2D extent, VkPipeline pipeline_state, VkPipelineLayout layout,
                           VkDescriptorSetLayout set_layout, const VkImageView* images, uint32_t image_count,
                           const Pass& constants, VkImage* target_image, VkImageLayout* target_layout,
                           VkImageLayout final_layout) {
    VkDescriptorSet set = descriptor(set_layout);
    if (!set)
        return;
    for (uint32_t i = 0; i < image_count; ++i)
        write_image(set, i, images[i], present_sampler);
    if (target_image)
        transition(buffer, *target_image, *target_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, extent};
    vkCmdBeginRenderPass(buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = metal_viewport(0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height));
    vkCmdSetViewport(buffer, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(buffer, 0, 1, &scissor);
    vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_state);
    vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(buffer, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
    vkCmdDraw(buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(buffer);
    if (target_image)
        *target_layout = final_layout;
}

void Gpu::Impl::bloom(VkCommandBuffer buffer) {
    Target& target = targets[newest];
    // Extract: the frame averaged down to the glow's size.
    const auto taps = static_cast<int32_t>(std::max<uint32_t>(1, width / (2 * kGlowWidth)));
    const Pass extract{{2.0f / static_cast<float>(width), 2.0f / static_cast<float>(height)}, taps, 0};
    fullscreen(buffer, glow_pass, glow_framebuffer[0], {kGlowWidth, kGlowHeight}, bloom_extract,
               composite_pipeline_layout, composite_layout, &target.view, 1, extract, &glow[0], &glow_layout[0],
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    fullscreen(buffer, glow_pass, glow_framebuffer[2], {kGlowWidth / 2, kGlowHeight / 2}, bloom_half,
               composite_pipeline_layout, composite_layout, &glow_view[0], 1, Pass{{0, 0}, 1, 0}, &glow[2],
               &glow_layout[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (int level = 0; level < 2; ++level) {
        const float w = static_cast<float>(kGlowWidth >> level), h = static_cast<float>(kGlowHeight >> level);
        fullscreen(buffer, glow_pass, glow_framebuffer[2 * level + 1], {kGlowWidth >> level, kGlowHeight >> level},
                   bloom_blur, composite_pipeline_layout, composite_layout, &glow_view[2 * level], 1,
                   Pass{{1.0f / w, 0}, 1, 0}, &glow[2 * level + 1], &glow_layout[2 * level + 1],
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        fullscreen(buffer, glow_pass, glow_framebuffer[2 * level], {kGlowWidth >> level, kGlowHeight >> level},
                   bloom_blur, composite_pipeline_layout, composite_layout, &glow_view[2 * level + 1], 1,
                   Pass{{0, 1.0f / h}, 1, 0}, &glow[2 * level], &glow_layout[2 * level],
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const VkImageView inputs[3] = {target.view, glow_view[0], glow_view[2]};
    fullscreen(buffer, shown_pass, shown_framebuffer, {width, height}, bloom_composite, composite_pipeline_layout,
               composite_layout, inputs, 3, Pass{{0, 0}, 1, kBloomStrength[std::min(bloom_level, 2u)]}, &shown,
               &shown_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Gpu::Impl::encode() {
    if (!bands.empty() && slices_map) {
        // A render that needs more than four bands doubles the pool once, as an explosion can need
        // many times the fragments of the render before.
        std::vector<Band> planned = plan_bands(slices_map, height / kSliceRows, node_capacity / 2);
        const uint32_t most = 2 * std::min(width * height * 4, kMaxPoolNodes);
        while (planned.size() > kMaxBands || (planned.size() > 4 && node_capacity < most)) {
            node_capacity *= 2;
            vkDestroyBuffer(device, nodes, nullptr);
            vkFreeMemory(device, nodes_memory, nullptr);
            nodes = VK_NULL_HANDLE;
            nodes_memory = VK_NULL_HANDLE;
            buffer(static_cast<VkDeviceSize>(node_capacity) * kNodeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nodes, nodes_memory);
            planned = plan_bands(slices_map, height / kSliceRows, node_capacity / 2);
        }
        bands = planned;
    }
    ++encodes;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Every pixel's list starts empty, and so does each band's count.
    vkCmdFillBuffer(cmd, heads, 0, static_cast<VkDeviceSize>(width) * height * 4, 0xFFFFFFFFu);
    vkCmdFillBuffer(cmd, farthest, 0, static_cast<VkDeviceSize>(width) * height * 4, 0xFFFFFFFFu);
    vkCmdFillBuffer(cmd, nearest, 0, static_cast<VkDeviceSize>(width) * height * 4, 0u);
    vkCmdFillBuffer(cmd, counters, 0, kMaxBands * 4, 0u);
    vkCmdFillBuffer(cmd, slices, 0, static_cast<VkDeviceSize>(height / kSliceRows) * 4, 0u);
    VkMemoryBarrier fill{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    fill.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fill.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &fill, 0,
                         nullptr, 0, nullptr);

    Target& target = targets[newest];
    if (!target.framebuffer) {
        image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
              target.image, target.memory);
        target.view = view(target.image, VK_FORMAT_R8G8B8A8_UNORM);
        image(width, height, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, target.depth,
              target.depth_memory);
        target.depth_view = view(target.depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, target.depth, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageView views[2] = {target.view, target.depth_view};
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = band_load_pass;
        fb.attachmentCount = 2;
        fb.pAttachments = views;
        fb.width = width;
        fb.height = height;
        fb.layers = 1;
        vkCreateFramebuffer(device, &fb, nullptr, &target.framebuffer);
        target.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    transition(cmd, target.image, target.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    target.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    const bool adding = std::any_of(draws.begin(), draws.end(), [](const Batch& d) { return d.adds; });
    PushConstants push{};
    push.screen.scale[0] = 2.0f / 640.0f;
    push.screen.scale[1] = 2.0f / 480.0f;
    push.screen.scale[2] = 1.0f / std::max(z_scale, 1e-6f);
    push.screen.scale[3] = 0;
    const float depth_clear = std::min(background[3] / std::max(z_scale, 1e-6f), 1.0f);
    for (size_t b = 0; b < bands.size(); ++b) {
        const Band& band = bands[b];
        VkClearValue clears[2]{};
        clears[0].color = {{background[0], background[1], background[2], 1.0f}};
        clears[1].depthStencil = {depth_clear, 0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = b == 0 ? band_clear_pass : band_load_pass;
        pass.framebuffer = target.framebuffer;
        pass.renderArea = {{0, band.y0}, {width, band.y1 - band.y0}};
        pass.clearValueCount = 2;
        pass.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        // The whole target, as Metal's default viewport is; the scissor below selects the band's rows.
        VkViewport viewport = metal_viewport(0, 0, static_cast<float>(width), static_cast<float>(height));
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, static_cast<int32_t>(band.y0)}, {width, band.y1 - band.y0}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        const VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &vertex_offset);
        auto strips = [&](bool add_strips) {
            for (const Batch& d : draws) {
                if (d.adds != add_strips)
                    continue;
                const int32_t y0 = std::max<int32_t>(d.scissor[1], static_cast<int32_t>(band.y0));
                const int32_t y1 = std::min<int32_t>(d.scissor[1] + d.scissor[3], static_cast<int32_t>(band.y1));
                if (y1 <= y0)
                    continue;
                VkPipeline p = VK_NULL_HANDLE;
                if (d.adds)
                    p = list_pipeline(add_strips ? added_top_pipelines : added_pipelines,
                                      add_strips ? fs_added_top : fs_added, d.depth, d.depth);
                else if (d.translucent)
                    p = list_pipeline(translucent_pipelines, fs_translucent, d.depth, d.depth);
                else
                    p = pipeline(d.pipeline | d.depth << 6);  // blend in bits 0-5, depth compare and write above
                if (p == VK_NULL_HANDLE)
                    continue;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                VkDescriptorSet set = descriptor(geometry_layout);
                if (!set)
                    continue;
                write_buffer(set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fog_buffer, sizeof(FogUniforms));
                write_image(set, 1, d.texture ? d.texture->view : white_texture.view, sampler(d.sampler));
                write_buffer(set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, heads,
                             static_cast<VkDeviceSize>(width) * height * 4);
                write_buffer(set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nodes,
                             static_cast<VkDeviceSize>(node_capacity) * kNodeBytes);
                write_buffer(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, counters, kMaxBands * 4);
                write_buffer(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slices,
                             static_cast<VkDeviceSize>(height / kSliceRows) * 4);
                write_buffer(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, farthest,
                             static_cast<VkDeviceSize>(width) * height * 4);
                write_buffer(set, 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nearest,
                             static_cast<VkDeviceSize>(width) * height * 4);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, geometry_pipeline_layout, 0, 1, &set,
                                        0, nullptr);
                PushConstants one = push;
                one.draw = d.uniforms;
                one.draw.band = static_cast<uint32_t>(b);
                vkCmdPushConstants(cmd, geometry_pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof one, &one);
                VkRect2D s{{d.scissor[0], y0},
                           {static_cast<uint32_t>(d.scissor[2]), static_cast<uint32_t>(y1 - y0)}};
                vkCmdSetScissor(cmd, 0, 1, &s);
                vkCmdDraw(cmd, d.count, 1, d.first, 0);
            }
        };
        strips(false);
        vkCmdEndRenderPass(cmd);

        if (adding) {
            // The adding strips see the lists that the others made, so they come in a pass of their own.
            VkRenderPassBeginInfo added_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            added_begin.renderPass = added_pass;
            added_begin.framebuffer = target.framebuffer;
            added_begin.renderArea = {{0, band.y0}, {width, band.y1 - band.y0}};
            vkCmdBeginRenderPass(cmd, &added_begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            strips(true);
            vkCmdEndRenderPass(cmd);
        }

        // The band's translucent fragments, sorted and blended over its opaque picture.
        transition(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cmd, scratch, scratch_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {0, static_cast<int32_t>(band.y0), 0};
        region.dstOffset = {0, static_cast<int32_t>(band.y0), 0};
        region.extent = {width, band.y1 - band.y0, 1};
        vkCmdCopyImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transition(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(cmd, scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scratch_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkRenderPassBeginInfo sort_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        sort_begin.renderPass = sort_pass;
        sort_begin.framebuffer = target.framebuffer;
        sort_begin.renderArea = {{0, band.y0}, {width, band.y1 - band.y0}};
        vkCmdBeginRenderPass(cmd, &sort_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sort);
        VkDescriptorSet sort_set = descriptor(sort_layout);
        if (sort_set) {
            write_buffer(sort_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, heads,
                         static_cast<VkDeviceSize>(width) * height * 4);
            write_buffer(sort_set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nodes,
                         static_cast<VkDeviceSize>(node_capacity) * kNodeBytes);
            write_buffer(sort_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slices,
                         static_cast<VkDeviceSize>(height / kSliceRows) * 4);
            write_image(sort_set, 3, scratch_view, present_sampler);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sort_pipeline_layout, 0, 1, &sort_set, 0,
                                    nullptr);
            const SortUniforms constants{width, node_capacity};
            vkCmdPushConstants(cmd, sort_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants,
                               &constants);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);

        if (adding) {
            VkRenderPassBeginInfo top_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            top_begin.renderPass = top_pass;
            top_begin.framebuffer = target.framebuffer;
            top_begin.renderArea = {{0, band.y0}, {width, band.y1 - band.y0}};
            vkCmdBeginRenderPass(cmd, &top_begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            auto top_strips = [&] {
                for (const Batch& d : draws) {
                    if (!d.adds)
                        continue;
                    const int32_t y0 = std::max<int32_t>(d.scissor[1], static_cast<int32_t>(band.y0));
                    const int32_t y1 = std::min<int32_t>(d.scissor[1] + d.scissor[3], static_cast<int32_t>(band.y1));
                    if (y1 <= y0)
                        continue;
                    VkPipeline p = list_pipeline(added_top_pipelines, fs_added_top, d.depth, d.depth);
                    if (p == VK_NULL_HANDLE)
                        continue;
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                    VkDescriptorSet set = descriptor(geometry_layout);
                    if (!set)
                        continue;
                    write_buffer(set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fog_buffer, sizeof(FogUniforms));
                    write_image(set, 1, d.texture ? d.texture->view : white_texture.view, sampler(d.sampler));
                    write_buffer(set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, heads,
                                 static_cast<VkDeviceSize>(width) * height * 4);
                    write_buffer(set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nodes,
                                 static_cast<VkDeviceSize>(node_capacity) * kNodeBytes);
                    write_buffer(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, counters, kMaxBands * 4);
                    write_buffer(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slices,
                                 static_cast<VkDeviceSize>(height / kSliceRows) * 4);
                    write_buffer(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, farthest,
                                 static_cast<VkDeviceSize>(width) * height * 4);
                    write_buffer(set, 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nearest,
                                 static_cast<VkDeviceSize>(width) * height * 4);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, geometry_pipeline_layout, 0, 1, &set,
                                            0, nullptr);
                    PushConstants one = push;
                    one.draw = d.uniforms;
                    one.draw.band = static_cast<uint32_t>(b);
                    vkCmdPushConstants(cmd, geometry_pipeline_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof one, &one);
                    VkRect2D s{{d.scissor[0], y0},
                               {static_cast<uint32_t>(d.scissor[2]), static_cast<uint32_t>(y1 - y0)}};
                    vkCmdSetScissor(cmd, 0, 1, &s);
                    vkCmdDraw(cmd, d.count, 1, d.first, 0);
                }
            };
            top_strips();
            vkCmdEndRenderPass(cmd);
        }
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vkQueueSubmit(graphics_queue, 1, &submit, fence) == VK_SUCCESS)
        fence_pending = true;
}

void Gpu::Impl::create_swapchain() {
    if (!surface) {
        // No window: no swapchain, but the pipelines that the render paths build against swap_pass.
        if (!swap_pass) {
            swap_format = VK_FORMAT_B8G8R8A8_UNORM;
            swap_pass = make_swap_pass(device, swap_format);
            swap_pass_format = swap_format;
        }
        return;
    }
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    uint32_t formats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formats, nullptr);
    std::vector<VkSurfaceFormatKHR> available(formats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formats, available.data());
    swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool format_chosen = false;
    for (const auto& f : available)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB) {
            swap_format = f.format;
            space = f.colorSpace;
            format_chosen = true;
            break;
        }
    if (!format_chosen)
        for (const auto& f : available)
            if (f.format != VK_FORMAT_UNDEFINED) {
                swap_format = f.format;
                space = f.colorSpace;
                break;
            }
    uint32_t modes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modes, nullptr);
    std::vector<VkPresentModeKHR> present_modes(modes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modes, present_modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!display_sync)
        for (VkPresentModeKHR m : present_modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                mode = m;
                break;
            }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    swap_extent = {static_cast<uint32_t>(std::max(1, pw)), static_cast<uint32_t>(std::max(1, ph))};
    if (caps.currentExtent.width != UINT32_MAX)
        swap_extent = caps.currentExtent;
    swap_extent.width = std::clamp(swap_extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    swap_extent.height = std::clamp(swap_extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    // The framebuffers below need the render pass at the format the surface actually chose. A
    // resize can change that format, in which case the two swapchain pipelines must follow it.
    if (!swap_pass || swap_format != swap_pass_format) {
        if (present) vkDestroyPipeline(device, present, nullptr);
        if (overlay) vkDestroyPipeline(device, overlay, nullptr);
        present = overlay = VK_NULL_HANDLE;
        if (swap_pass) vkDestroyRenderPass(device, swap_pass, nullptr);
        swap_pass = make_swap_pass(device, swap_format);
        swap_pass_format = swap_format;
        if (swap_pass && vs_present && fs_present && present_pipeline_layout) {
            present = build(vs_present, fs_present, swap_pass, present_pipeline_layout, false, false, false,
                            VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_COMPARE_OP_ALWAYS, false, true);
            overlay = build(vs_present, fs_present, swap_pass, present_pipeline_layout, false, false, true,
                            VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_COMPARE_OP_ALWAYS, false,
                            true);
        }
    }
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = count;
    info.imageFormat = swap_format;
    info.imageColorSpace = space;
    info.imageExtent = swap_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const uint32_t families[2] = {graphics_family, present_family};
    if (graphics_family != present_family) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    }
    else
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device, &info, nullptr, &swapchain) != VK_SUCCESS)
        return;
    uint32_t images = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &images, nullptr);
    swap_images.resize(images);
    vkGetSwapchainImagesKHR(device, swapchain, &images, swap_images.data());
    swap_views.resize(images);
    swap_framebuffers.resize(images);
    for (uint32_t i = 0; i < images; ++i) {
        swap_views[i] = view(swap_images[i], swap_format);
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = swap_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &swap_views[i];
        fb.width = swap_extent.width;
        fb.height = swap_extent.height;
        fb.layers = 1;
        vkCreateFramebuffer(device, &fb, nullptr, &swap_framebuffers[i]);
    }
}

void Gpu::Impl::destroy_swapchain() {
    for (VkFramebuffer fb : swap_framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    for (VkImageView v : swap_views) vkDestroyImageView(device, v, nullptr);
    swap_framebuffers.clear();
    swap_views.clear();
    swap_images.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

namespace {

// A render pass for the swapchain: its image starts undefined and ends as the present source.
VkRenderPass make_swap_pass(VkDevice device, VkFormat format) {
    VkAttachmentDescription colour{};
    colour.format = format;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colour.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &colour;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    VkRenderPass out = VK_NULL_HANDLE;
    vkCreateRenderPass(device, &info, nullptr, &out);
    return out;
}

}  // namespace

Gpu::Gpu() : impl_(std::make_unique<Impl>()) {}

Gpu::~Gpu() {
    if (!impl_ || !impl_->device)
        return;
    Impl& m = *impl_;
    vkDeviceWaitIdle(m.device);
    for (auto& [key, p] : m.pipelines) vkDestroyPipeline(m.device, p, nullptr);
    for (auto& [key, p] : m.translucent_pipelines) vkDestroyPipeline(m.device, p, nullptr);
    for (auto& [key, p] : m.added_pipelines) vkDestroyPipeline(m.device, p, nullptr);
    for (auto& [key, p] : m.added_top_pipelines) vkDestroyPipeline(m.device, p, nullptr);
    for (VkPipeline p : {m.sort, m.present, m.overlay, m.bloom_extract, m.bloom_half, m.bloom_blur, m.bloom_composite})
        if (p) vkDestroyPipeline(m.device, p, nullptr);
    for (auto& [key, s] : m.samplers) vkDestroySampler(m.device, s, nullptr);
    if (m.present_sampler) vkDestroySampler(m.device, m.present_sampler, nullptr);
    m.destroy_targets();
    for (auto& [key, t] : m.textures) {
        if (t.view) vkDestroyImageView(m.device, t.view, nullptr);
        if (t.image) vkDestroyImage(m.device, t.image, nullptr);
        if (t.memory) vkFreeMemory(m.device, t.memory, nullptr);
    }
    if (m.white_texture.view) vkDestroyImageView(m.device, m.white_texture.view, nullptr);
    if (m.white_texture.image) vkDestroyImage(m.device, m.white_texture.image, nullptr);
    if (m.white_texture.memory) vkFreeMemory(m.device, m.white_texture.memory, nullptr);
    for (auto& o : m.overlays) {
        if (o.view) vkDestroyImageView(m.device, o.view, nullptr);
        if (o.image) vkDestroyImage(m.device, o.image, nullptr);
        if (o.memory) vkFreeMemory(m.device, o.memory, nullptr);
    }
    for (VkBuffer b : {m.heads, m.nodes, m.counters, m.slices, m.farthest, m.nearest, m.vertices, m.fog_buffer})
        if (b) vkDestroyBuffer(m.device, b, nullptr);
    for (VkDeviceMemory mem : {m.heads_memory, m.nodes_memory, m.counters_memory, m.slices_memory, m.farthest_memory,
                               m.nearest_memory, m.vertices_memory, m.fog_memory})
        if (mem) vkFreeMemory(m.device, mem, nullptr);
    if (m.descriptor_pool) vkDestroyDescriptorPool(m.device, m.descriptor_pool, nullptr);
    for (VkDescriptorSetLayout layout : {m.geometry_layout, m.sort_layout, m.present_layout, m.composite_layout})
        if (layout) vkDestroyDescriptorSetLayout(m.device, layout, nullptr);
    for (VkPipelineLayout layout : {m.geometry_pipeline_layout, m.sort_pipeline_layout, m.present_pipeline_layout,
                                    m.composite_pipeline_layout})
        if (layout) vkDestroyPipelineLayout(m.device, layout, nullptr);
    for (VkRenderPass pass : {m.band_clear_pass, m.band_load_pass, m.added_pass, m.sort_pass, m.top_pass, m.glow_pass,
                              m.shown_pass, m.swap_pass})
        if (pass) vkDestroyRenderPass(m.device, pass, nullptr);
    for (VkShaderModule shader : {m.vs_geometry, m.fs_geometry, m.fs_translucent, m.fs_added, m.fs_added_top, m.fs_sort,
                                  m.vs_present, m.fs_present, m.fs_bloom_extract, m.fs_bloom_blur,
                                  m.fs_bloom_composite})
        if (shader) vkDestroyShaderModule(m.device, shader, nullptr);
    m.destroy_swapchain();
    if (m.image_available) vkDestroySemaphore(m.device, m.image_available, nullptr);
    if (m.render_finished) vkDestroySemaphore(m.device, m.render_finished, nullptr);
    if (m.fence) vkDestroyFence(m.device, m.fence, nullptr);
    if (m.command_pool) vkDestroyCommandPool(m.device, m.command_pool, nullptr);
    vkDestroyDevice(m.device, nullptr);
    if (m.surface) vkDestroySurfaceKHR(m.instance, m.surface, nullptr);
    if (m.debug) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(m.instance, m.debug, nullptr);
    }
    if (m.instance) vkDestroyInstance(m.instance, nullptr);
}

bool Gpu::create(SDL_Window* window, unsigned scale, std::string& error) {
    Impl& m = *impl_;
    m.window = window;
    m.scale = std::max(1u, scale);
    m.width = 640 * m.scale;
    m.height = 480 * m.scale;

    // A run without a window (render-view, tests) has no surface: it needs no SDL extension, no
    // present queue and no swapchain, and only draws into the targets that read_displayed() reads.
    std::vector<const char*> extensions;
    if (window) {
        uint32_t sdl_extensions = 0;
        const char* const* sdl_names = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions);
        if (!sdl_names || !sdl_extensions) {
            error = std::string("SDL has no Vulkan loader: ") + SDL_GetError();
            return false;
        }
        extensions.assign(sdl_names, sdl_names + sdl_extensions);
    }
    uint32_t available_extensions = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions, nullptr);
    std::vector<VkExtensionProperties> extension_properties(available_extensions);
    vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions, extension_properties.data());
    const bool debug_utils = std::any_of(extension_properties.begin(), extension_properties.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
    });
    if (debug_utils)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // Validation is opt-in: a normal run should not pay for it, but a smoke test needs its messages.
    std::vector<const char*> layers;
    const char* validation = std::getenv("IKARUGA_VULKAN_VALIDATION");
    if (validation && std::strcmp(validation, "0") != 0) {
        uint32_t available_layers = 0;
        vkEnumerateInstanceLayerProperties(&available_layers, nullptr);
        std::vector<VkLayerProperties> layer_properties(available_layers);
        vkEnumerateInstanceLayerProperties(&available_layers, layer_properties.data());
        const bool found = std::any_of(layer_properties.begin(), layer_properties.end(), [](const auto& l) {
            return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (found)
            layers.push_back("VK_LAYER_KHRONOS_validation");
        else
            std::fprintf(stderr, "render: validation requested, but VK_LAYER_KHRONOS_validation is not installed\n");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Ikaruga";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "ikaruga-decomp";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    instance_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    const VkResult ir = vkCreateInstance(&instance_info, nullptr, &m.instance);
    if (ir != VK_SUCCESS) {
        error = std::string("no Vulkan instance: ") + vk_result(ir);
        return false;
    }
    if (window && !SDL_Vulkan_CreateSurface(window, m.instance, nullptr, &m.surface)) {
        error = std::string("no Vulkan surface: ") + SDL_GetError();
        return false;
    }

    uint32_t devices = 0;
    vkEnumeratePhysicalDevices(m.instance, &devices, nullptr);
    if (!devices) {
        error = "no Vulkan device";
        return false;
    }
    std::vector<VkPhysicalDevice> candidates(devices);
    vkEnumeratePhysicalDevices(m.instance, &devices, candidates.data());
    for (VkPhysicalDevice candidate : candidates) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queue_families.data());
        int graphics = -1, present = -1;
        for (uint32_t i = 0; i < families; ++i) {
            VkBool32 supported = VK_FALSE;
            if (m.surface)
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, m.surface, &supported);
            else
                supported = (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? VK_TRUE : VK_FALSE;
            if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics < 0)
                graphics = static_cast<int>(i);
            if (supported && present < 0)
                present = static_cast<int>(i);
        }
        if (graphics >= 0 && present >= 0) {
            m.physical = candidate;
            m.graphics_family = static_cast<uint32_t>(graphics);
            m.present_family = static_cast<uint32_t>(present);
            break;
        }
    }
    if (!m.physical) {
        error = "no Vulkan device with a graphics and a present queue";
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(m.physical, &m.memory_properties);
    vkGetPhysicalDeviceProperties(m.physical, &m.properties);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(m.physical, &features);
    m.sampler_anisotropy = features.samplerAnisotropy == VK_TRUE;

    const uint32_t families[2] = {m.graphics_family, m.present_family};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queues[2]{};
    queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[0].queueFamilyIndex = m.graphics_family;
    queues[0].queueCount = 1;
    queues[0].pQueuePriorities = &priority;
    queues[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[1].queueFamilyIndex = m.present_family;
    queues[1].queueCount = 1;
    queues[1].pQueuePriorities = &priority;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = m.graphics_family == m.present_family ? 1 : 2;
    device_info.pQueueCreateInfos = queues;
    device_info.enabledExtensionCount = m.surface ? 1 : 0;
    device_info.ppEnabledExtensionNames = device_extensions;
    device_info.pEnabledFeatures = &features;
    if (vkCreateDevice(m.physical, &device_info, nullptr, &m.device) != VK_SUCCESS) {
        error = "no Vulkan device";
        return false;
    }
    vkGetDeviceQueue(m.device, m.graphics_family, 0, &m.graphics_queue);
    vkGetDeviceQueue(m.device, m.present_family, 0, &m.present_queue);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = m.graphics_family;
    vkCreateCommandPool(m.device, &pool, nullptr, &m.command_pool);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = m.command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    vkAllocateCommandBuffers(m.device, &allocate, &m.cmd);
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m.device, &fence, nullptr, &m.fence);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(m.device, &semaphore, nullptr, &m.image_available);
    vkCreateSemaphore(m.device, &semaphore, nullptr, &m.render_finished);

    m.vs_geometry = m.module(render::shaders::geometry_vert, sizeof render::shaders::geometry_vert);
    m.fs_geometry = m.module(render::shaders::geometry_frag, sizeof render::shaders::geometry_frag);
    m.fs_translucent = m.module(render::shaders::translucent_frag, sizeof render::shaders::translucent_frag);
    m.fs_added = m.module(render::shaders::added_frag, sizeof render::shaders::added_frag);
    m.fs_added_top = m.module(render::shaders::added_top_frag, sizeof render::shaders::added_top_frag);
    m.fs_sort = m.module(render::shaders::sort_frag, sizeof render::shaders::sort_frag);
    m.vs_present = m.module(render::shaders::present_vert, sizeof render::shaders::present_vert);
    m.fs_present = m.module(render::shaders::present_frag, sizeof render::shaders::present_frag);
    m.fs_bloom_extract = m.module(render::shaders::bloom_extract_frag, sizeof render::shaders::bloom_extract_frag);
    m.fs_bloom_blur = m.module(render::shaders::bloom_blur_frag, sizeof render::shaders::bloom_blur_frag);
    m.fs_bloom_composite =
        m.module(render::shaders::bloom_composite_frag, sizeof render::shaders::bloom_composite_frag);
    if (!m.vs_geometry || !m.fs_geometry || !m.vs_present) {
        error = "the shaders do not build";
        return false;
    }

    VkDescriptorSetLayoutBinding geometry_bindings[8]{};
    geometry_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    geometry_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 2; i < 8; ++i)
        geometry_bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 8;
    set_info.pBindings = geometry_bindings;
    vkCreateDescriptorSetLayout(m.device, &set_info, nullptr, &m.geometry_layout);

    VkDescriptorSetLayoutBinding sort_bindings[4]{};
    for (uint32_t i = 0; i < 3; ++i)
        sort_bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    sort_bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    set_info.bindingCount = 4;
    set_info.pBindings = sort_bindings;
    vkCreateDescriptorSetLayout(m.device, &set_info, nullptr, &m.sort_layout);

    VkDescriptorSetLayoutBinding present_bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i)
        present_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    set_info.bindingCount = 1;
    set_info.pBindings = present_bindings;
    vkCreateDescriptorSetLayout(m.device, &set_info, nullptr, &m.present_layout);
    set_info.bindingCount = 3;
    vkCreateDescriptorSetLayout(m.device, &set_info, nullptr, &m.composite_layout);

    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
    };
    VkDescriptorPoolCreateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptor_info.maxSets = 16384;
    descriptor_info.poolSizeCount = 3;
    descriptor_info.pPoolSizes = sizes;
    vkCreateDescriptorPool(m.device, &descriptor_info, nullptr, &m.descriptor_pool);

    VkPushConstantRange geometry_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &m.geometry_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &geometry_range;
    vkCreatePipelineLayout(m.device, &layout_info, nullptr, &m.geometry_pipeline_layout);
    VkPushConstantRange sort_range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SortUniforms)};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &m.sort_layout;
    layout_info.pPushConstantRanges = &sort_range;
    vkCreatePipelineLayout(m.device, &layout_info, nullptr, &m.sort_pipeline_layout);
    VkPushConstantRange present_range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Pass)};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &m.present_layout;
    layout_info.pPushConstantRanges = &present_range;
    vkCreatePipelineLayout(m.device, &layout_info, nullptr, &m.present_pipeline_layout);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &m.composite_layout;
    vkCreatePipelineLayout(m.device, &layout_info, nullptr, &m.composite_pipeline_layout);

    m.band_clear_pass =
        m.render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                      true, VK_ATTACHMENT_LOAD_OP_CLEAR);
    m.band_load_pass =
        m.render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                      true, VK_ATTACHMENT_LOAD_OP_CLEAR);
    m.added_pass =
        m.render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                      true, VK_ATTACHMENT_LOAD_OP_LOAD);
    m.top_pass =
        m.render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                      true, VK_ATTACHMENT_LOAD_OP_LOAD);
    // The sort pass shares the target's framebuffer, which carries both colour and depth, so its
    // render pass has to declare the depth attachment even though the sort pipeline does not test it.
    m.sort_pass =
        m.render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                      true, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    m.glow_pass = m.render_pass(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_ATTACHMENT_LOAD_OP_DONT_CARE, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    m.shown_pass =
        m.render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ATTACHMENT_LOAD_OP_DONT_CARE, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m.device, &sampler_info, nullptr, &m.present_sampler);

    m.make_white();
    m.buffer(sizeof(FogUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m.fog_buffer, m.fog_memory);
    vkMapMemory(m.device, m.fog_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m.fog_map));
    m.buffer(1 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m.vertices,
             m.vertices_memory);
    m.vertices_size = 1 << 20;
    vkMapMemory(m.device, m.vertices_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m.vertex_map));

    m.size_buffers();
    m.create_swapchain();

    m.sort = m.build(m.vs_present, m.fs_sort, m.sort_pass, m.sort_pipeline_layout, false, false, false,
                     VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_COMPARE_OP_ALWAYS, false, true);
    m.present = m.build(m.vs_present, m.fs_present, m.swap_pass, m.present_pipeline_layout, false, false, false,
                        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_COMPARE_OP_ALWAYS, false, true);
    m.overlay = m.build(m.vs_present, m.fs_present, m.swap_pass, m.present_pipeline_layout, false, false, true,
                        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_COMPARE_OP_ALWAYS, false, true);
    m.bloom_extract = m.build(m.vs_present, m.fs_bloom_extract, m.glow_pass, m.composite_pipeline_layout, false,
                              false, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_COMPARE_OP_ALWAYS, false,
                              true);
    m.bloom_half = m.build(m.vs_present, m.fs_present, m.glow_pass, m.composite_pipeline_layout, false, false, false,
                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_COMPARE_OP_ALWAYS, false, true);
    m.bloom_blur = m.build(m.vs_present, m.fs_bloom_blur, m.glow_pass, m.composite_pipeline_layout, false, false,
                           false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_COMPARE_OP_ALWAYS, false, true);
    m.bloom_composite = m.build(m.vs_present, m.fs_bloom_composite, m.shown_pass, m.composite_pipeline_layout, false,
                                false, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_COMPARE_OP_ALWAYS,
                                false, true);
    return true;
}

const char* Gpu::driver() const {
    return impl_->physical ? impl_->properties.deviceName : "none";
}

void Gpu::last_render(double& gpu_ms, uint32_t& fragments) {
    impl_->wait();
    gpu_ms = impl_->last_gpu_ms;
    fragments = 0;
    if (impl_->counters_map)
        for (size_t b = 0; b < impl_->bands.size(); ++b) fragments += impl_->counters_map[b];
}

void Gpu::render(const DisplayList& list, const Hardware& hw) { render(translate(list, hw), hw); }

void Gpu::render(const Frame& frame, const Hardware& hw) {
    Impl& m = *impl_;
    if (!m.device)
        return;
    m.wait();
    vkResetDescriptorPool(m.device, m.descriptor_pool, 0);
    const uint64_t render = renders + 1;
    float z = std::max(frame.max_z, frame.background_depth);
    if (!(z > 0.0f))
        z = 1.0f;
    m.z_scale = z;
    const uint32_t argb = frame.background;
    m.background[0] = ((argb >> 16) & 255) / 255.0f;
    m.background[1] = ((argb >> 8) & 255) / 255.0f;
    m.background[2] = (argb & 255) / 255.0f;
    m.background[3] = frame.background_depth;

    m.draws.clear();
    m.triangles.clear();
    for (List which : {List::Opaque, List::PunchThrough, List::Translucent}) {
        uint32_t index = 0;
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
                if (!s.gouraud) {
                    t[0].base = t[1].base = t[2].base;
                    t[0].offset = t[1].offset = t[2].offset;
                }
                m.triangles.insert(m.triangles.end(), t, t + 3);
            }
            d.count = static_cast<uint32_t>(m.triangles.size()) - d.first;
            d.translucent = which == List::Translucent;
            d.pipeline = static_cast<uint32_t>(s.source) | static_cast<uint32_t>(s.target) << 3;
            const uint32_t source = d.pipeline & 7, target = d.pipeline >> 3;
            d.adds = d.translucent && target == 1 && (source == 0 || source == 1 || source == 4 || source == 5);
            if (d.adds && source == 0)
                continue;
            const bool write = s.depth_write && !d.translucent;
            d.depth = static_cast<uint32_t>(s.depth) | write << 3;
            if (s.textured)
                d.texture = &m.texture(s, frame, hw, render, textures_decoded, untextured);
            else
                d.texture = &m.white_texture;
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
            d.scissor[0] = x0 * static_cast<int>(m.scale);
            d.scissor[1] = y0 * static_cast<int>(m.scale);
            d.scissor[2] = (x1 - x0) * static_cast<int>(m.scale);
            d.scissor[3] = (y1 - y0) * static_cast<int>(m.scale);
            d.uniforms.flags = s.textured | s.offset_colour << 1 | s.use_alpha << 2 | s.ignore_texture_alpha << 3 |
                               (which == List::PunchThrough) << 4;
            d.uniforms.shading = static_cast<int32_t>(s.shading);
            d.uniforms.fog = static_cast<int32_t>(s.fog);
            d.uniforms.blend = d.pipeline;
            d.uniforms.alpha_ref = frame.alpha_ref;
            d.uniforms.order = d.translucent ? order : 0;
            d.uniforms.lod_bias = s.mipmap_bias;
            d.uniforms.width = m.width;
            d.uniforms.capacity = m.node_capacity;
            if (!d.count || d.scissor[2] <= 0 || d.scissor[3] <= 0)
                continue;
            if (!d.translucent && !m.draws.empty()) {
                Batch& last = m.draws.back();
                if (!last.translucent && last.first + last.count == d.first && last.pipeline == d.pipeline &&
                    last.depth == d.depth && last.texture == d.texture && last.sampler == d.sampler &&
                    last.scissor[0] == d.scissor[0] && last.scissor[1] == d.scissor[1] &&
                    last.scissor[2] == d.scissor[2] && last.scissor[3] == d.scissor[3] &&
                    std::memcmp(&last.uniforms, &d.uniforms, sizeof d.uniforms) == 0) {
                    last.count += d.count;
                    continue;
                }
            }
            m.draws.push_back(d);
        }
    }

    const size_t vertex_bytes = m.triangles.size() * sizeof(Vertex);
    if (vertex_bytes > m.vertices_size) {
        vkUnmapMemory(m.device, m.vertices_memory);
        vkDestroyBuffer(m.device, m.vertices, nullptr);
        vkFreeMemory(m.device, m.vertices_memory, nullptr);
        m.vertices_size = std::max<size_t>(vertex_bytes * 2, 1 << 20);
        m.buffer(m.vertices_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m.vertices,
                 m.vertices_memory);
        vkMapMemory(m.device, m.vertices_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m.vertex_map));
    }
    if (vertex_bytes)
        std::memcpy(m.vertex_map, m.triangles.data(), vertex_bytes);

    std::memset(m.fog_map, 0, sizeof(FogUniforms));
    colour_of(frame.fog_table_colour, m.fog_map->table_colour);
    colour_of(frame.fog_vertex_colour, m.fog_map->vertex_colour);
    m.fog_map->params[0] = frame.fog_density;
    m.fog_map->params[1] = z;
    for (int i = 0; i < 128; ++i) {
        m.fog_map->table[i / 2][(i & 1) * 2] = static_cast<float>(frame.fog_table[i][0]) / 255.0f;
        m.fog_map->table[i / 2][(i & 1) * 2 + 1] = static_cast<float>(frame.fog_table[i][1]) / 255.0f;
    }
    m.newest = frame.target;
    m.newest_valid = true;
    const double start = m.frame_start;
    m.frame_start = 0;
    m.encode();
    m.last_gpu_ms = 0;
    if (start > 0) {
        m.wait();
        m.work = now() - start;
    }
    ++renders;
    ++m.renders_total;
    if (renders % 600 == 0)
        for (auto it = m.textures.begin(); it != m.textures.end();)
            it = renders > 3600 && it->second.used + 3600 < renders ? m.textures.erase(it) : std::next(it);
}

void Gpu::present(const Hardware& hw, double refresh, bool white, double mark) {
    Impl& m = *impl_;
    if (!m.window || !m.device)
        return;
    m.wait();
    if (!m.swapchain)
        m.create_swapchain();
    if (!m.swapchain)
        return;
    uint32_t image = 0;
    VkResult acquired = vkAcquireNextImageKHR(m.device, m.swapchain, UINT64_MAX, m.image_available, VK_NULL_HANDLE,
                                              &image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        m.destroy_swapchain();
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        return;
    vkResetCommandBuffer(m.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m.cmd, &begin);

    const uint32_t shown_target = m.newest_valid ? m.newest : (hw.registers[kFbRSof1] & 0xFFFFFF);
    auto it = m.targets.find(shown_target);
    bool bloom_ready = false;
    if (it != m.targets.end() && !white) {
        m.transition(m.cmd, it->second.image, it->second.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        it->second.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (it != m.targets.end() && !white && m.bloom_level > 0) {
        m.newest = shown_target;
        m.bloom(m.cmd);
        bloom_ready = true;
    }

    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = m.swap_pass;
    pass.framebuffer = m.swap_framebuffers[image];
    pass.renderArea = {{0, 0}, m.swap_extent};
    VkClearValue clear{};
    clear.color = white ? VkClearColorValue{{1, 1, 1, 1}} : VkClearColorValue{{0, 0, 0, 1}};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(m.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (!white && it != m.targets.end()) {
        const VkImageView input = bloom_ready ? m.shown_view : it->second.view;
        VkDescriptorSet set = m.descriptor(m.present_layout);
        m.write_image(set, 0, input, m.present_sampler);
        double w = m.swap_extent.width, h = w * 3.0 / 4.0;
        if (h > m.swap_extent.height) {
            h = m.swap_extent.height;
            w = h * 4.0 / 3.0;
        }
        const int taps = std::clamp(static_cast<int>(std::ceil(static_cast<double>(m.height) / h - 0.01)), 1, 8);
        const Pass p{{static_cast<float>(1.0 / (w * taps)), static_cast<float>(1.0 / (h * taps))}, taps, 0};
        VkViewport viewport = metal_viewport(static_cast<float>((m.swap_extent.width - w) / 2),
                                             static_cast<float>((m.swap_extent.height - h) / 2),
                                             static_cast<float>(w), static_cast<float>(h));
        vkCmdSetViewport(m.cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, m.swap_extent};
        vkCmdSetScissor(m.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.present);
        vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.present_pipeline_layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(m.cmd, m.present_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        vkCmdDraw(m.cmd, 3, 1, 0, 0);
    }
    if (!white && m.overlay_shown && m.overlay_shown->width == m.swap_extent.width &&
        m.overlay_shown->height == m.swap_extent.height) {
        VkDescriptorSet set = m.descriptor(m.present_layout);
        m.write_image(set, 0, m.overlay_shown->view, m.present_sampler);
        const Pass p{{0, 0}, 1, 0};
        VkViewport viewport = metal_viewport(0, 0, static_cast<float>(m.swap_extent.width),
                                             static_cast<float>(m.swap_extent.height));
        vkCmdSetViewport(m.cmd, 0, 1, &viewport);
        vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.overlay);
        vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.present_pipeline_layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(m.cmd, m.present_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        vkCmdDraw(m.cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(m.cmd);
    vkEndCommandBuffer(m.cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m.render_finished;
    if (vkQueueSubmit(m.graphics_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        return;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m.render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &m.swapchain;
    present.pImageIndices = &image;
    const VkResult r = vkQueuePresentKHR(m.present_queue, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkQueueWaitIdle(m.present_queue);
        m.destroy_swapchain();
    }
    else
        vkQueueWaitIdle(m.present_queue);
    m.presented_time = now();
    if (refresh > 0 && m.presented_time > refresh + refresh_period() / 2)
        ++m.late_presents;
    (void)mark;
}

void Gpu::start_frame(double t) { impl_->frame_start = t; }

double Gpu::take_work() {
    const double w = impl_->work;
    impl_->work = 0;
    return w;
}

void Gpu::set_display_sync(bool on) {
    Impl& m = *impl_;
    if (m.display_sync == on)
        return;
    m.display_sync = on;
    m.wait();
    m.destroy_swapchain();
}

double Gpu::presented() const { return impl_->presented_time; }
uint64_t Gpu::late() const { return impl_->late_presents; }

double Gpu::refresh_period() const {
    const SDL_DisplayMode* mode =
        impl_->window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(impl_->window)) : nullptr;
    return mode && mode->refresh_rate > 0 ? 1.0 / mode->refresh_rate : 1.0 / 60;
}

double Gpu::now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Gpu::set_overlay(const uint32_t* pixels, uint32_t width, uint32_t height) {
    Impl& m = *impl_;
    if (!pixels || !width || !height || !m.device) {
        m.overlay_shown = nullptr;
        return;
    }
    m.wait();
    Impl::Overlay& o = m.overlays[m.overlay_next];
    m.overlay_next ^= 1;
    if (!o.image || o.width != width || o.height != height) {
        if (o.view) vkDestroyImageView(m.device, o.view, nullptr);
        if (o.image) vkDestroyImage(m.device, o.image, nullptr);
        if (o.memory) vkFreeMemory(m.device, o.memory, nullptr);
        m.image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, o.image, o.memory);
        o.view = m.view(o.image, VK_FORMAT_R8G8B8A8_UNORM);
        o.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        o.width = width;
        o.height = height;
    }
    CachedTexture temporary;
    temporary.image = o.image;
    temporary.memory = o.memory;
    temporary.view = o.view;
    temporary.layout = o.layout;
    m.upload_image(temporary, 0, width, height, std::vector<uint32_t>(pixels, pixels + static_cast<size_t>(width) * height));
    o.layout = temporary.layout;
    m.overlay_shown = &o;
}

bool Gpu::read_displayed(const Hardware& hw, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height,
                         bool bloomed) {
    return read_target(hw.registers[kFbRSof1] & 0xFFFFFF, pixels, width, height, bloomed);
}

unsigned Gpu::scale() const { return impl_->scale; }

void Gpu::set_scale(unsigned scale) {
    Impl& m = *impl_;
    scale = std::max(1u, scale);
    if (!m.device || scale == m.scale)
        return;
    m.wait();
    m.scale = scale;
    m.width = 640 * scale;
    m.height = 480 * scale;
    m.size_buffers();
    std::printf("render: drawing at %ux%u\n", m.width, m.height);
}

unsigned Gpu::anisotropy() const { return impl_->anisotropy; }

void Gpu::set_anisotropy(unsigned samples) {
    samples = std::clamp(samples, 1u, 16u);
    if (samples != impl_->anisotropy) {
        impl_->anisotropy = samples;
        impl_->wait();
        for (auto& [key, s] : impl_->samplers) vkDestroySampler(impl_->device, s, nullptr);
        impl_->samplers.clear();
    }
}

void Gpu::set_english(bool on) { impl_->english = on; }

void Gpu::set_hud(bool on) {
    impl_->hud = on;
    // Vulkan has no per-layer HUD; the frame counter still shows with F10.
    (void)on;
}

void Gpu::set_bloom(unsigned level) { impl_->bloom_level = std::min(level, 2u); }

bool Gpu::read_target(uint32_t address, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height,
                      bool bloomed) {
    Impl& m = *impl_;
    auto it = m.targets.find(address);
    if (it == m.targets.end() || !m.device)
        return false;
    m.wait();
    if (bloomed && m.bloom_level) {
        VkCommandBuffer one = m.begin_once();
        m.newest = address;
        m.transition(one, it->second.image, it->second.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        it->second.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m.bloom(one);
        vkEndCommandBuffer(one);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &one;
        vkQueueSubmit(m.graphics_queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(m.graphics_queue);
        vkFreeCommandBuffers(m.device, m.command_pool, 1, &one);
        const bool ok = m.read_image(m.shown, m.width, m.height, m.shown_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     pixels, width, height);
        m.shown_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return ok;
    }
    const bool ok = m.read_image(it->second.image, m.width, m.height, it->second.layout,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pixels, width, height);
    it->second.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return ok;
}

bool Gpu::read_texture(uint32_t tcw, uint32_t tsp, uint32_t palette_format, std::vector<uint32_t>& pixels,
                       uint32_t& width, uint32_t& height) {
    TextureFormat f;
    if (!describe_texture(tcw, tsp, 0, f))
        return false;
    auto it = impl_->textures.find(texture_key(f, palette_format & 3));
    if (it == impl_->textures.end())
        return false;
    return impl_->read_image(it->second.image, it->second.width, it->second.height, it->second.layout,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pixels, width, height);
}

}  // namespace render
