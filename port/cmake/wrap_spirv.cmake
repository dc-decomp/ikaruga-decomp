# Wraps glslc's -mfmt=c output (a bare brace-enclosed list of words) in a C++ declaration.
#   cmake -DIN=shader.spv.c -DOUT=shader.h -DSYM=shader_vert -P wrap_spirv.cmake
file(READ ${IN} _words)
file(WRITE ${OUT}
     "// Generated from a GLSL shader by glslc. Do not edit.\n"
     "#pragma once\n#include <cstdint>\n"
     "namespace render::shaders {\n"
     "static const std::uint32_t ${SYM}[] = ${_words};\n"
     "}  // namespace render::shaders\n")
