// The renderer's shaders: the Metal source in shaders/geometry.metal, which the renderer compiles
// when it starts (port/CMakeLists.txt embeds it).
#pragma once

#include <cstddef>
#include <cstdint>

namespace render::shaders {
extern const uint8_t geometry_metal[];  // ends with a 0 that the size leaves out
extern const size_t geometry_metal_size;
}  // namespace render::shaders
