# The native port: Ikaruga's decompiled code on a native runtime, without dream-recomp.
#
#   cmake -S port -B build-port -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-port
#
# The decompiled code is generated from the user's disc by tools/ghidra/analyze.sh and
# tools/decomp/convert.py into decomp/ (IKARUGA_DECOMP_DIR), which stays out of this repository.
cmake_minimum_required(VERSION 3.24)
project(ikaruga_port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(IKARUGA_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../decomp" CACHE PATH
    "Directory with the converted decompiled code (gen/ and fix/)")

file(GLOB DECOMP_SOURCES CONFIGURE_DEPENDS
     "${IKARUGA_DECOMP_DIR}/gen/src/*.cpp"
     "${IKARUGA_DECOMP_DIR}/fix/*.cpp")

# The decompiled code relies on the SH-4's arithmetic: signed overflow wraps, and memory is read
# through any type. Floats round as the SH-4 rounds them: a multiply and an add stay two operations,
# and the rounding mode is FPSCR's, set at run time (cpu::set_fpscr). Stage 1's scroll stops 0.02
# past the position where its script waits, so a difference in the last bit can stop a stage.
# Constants are folded at compile time, as SHC folded them: Ghidra prints an integer in a float
# slot as a denormal literal, which a conversion at run time flushes to 0 under FPSCR.DN.
add_library(ikaruga_decomp STATIC ${DECOMP_SOURCES})
target_include_directories(ikaruga_decomp PUBLIC include "${IKARUGA_DECOMP_DIR}/gen/include")
target_compile_options(ikaruga_decomp PRIVATE -fwrapv -fno-strict-aliasing -w -O1 -g
                       -ferror-limit=0 -ffp-contract=off)

# The console: dream-recomp's runtime (memory map, scheduler, interrupt controllers, PowerVR, DMA,
# Maple, AICA with its ARM7). Only the libraries the port links are built.
set(DREAM_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dream-recomp" CACHE PATH "The dream-recomp checkout")
set(DREAM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DREAM_DEV_INTERPRETER OFF CACHE BOOL "" FORCE)
add_subdirectory("${DREAM_RECOMP_DIR}" dream EXCLUDE_FROM_ALL)

# The renderer: the tile accelerator's parameter stream, decoded and drawn natively with Metal, in
# an SDL3 window. Its shaders are Metal source, which it compiles when it starts.
enable_language(OBJCXX)
find_package(SDL3 CONFIG REQUIRED)
set(SHADER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/geometry.metal")
add_custom_command(OUTPUT "${SHADER_BUILD_DIR}/shaders.cpp"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_BUILD_DIR}"
  COMMAND ${CMAKE_COMMAND} "-DOUTPUT=${SHADER_BUILD_DIR}/shaders.cpp" "-DINPUTS=${SHADER_SOURCE}=geometry_metal"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS "${SHADER_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake" VERBATIM)

add_library(ikaruga_render STATIC
  src/render/ta.cpp
  src/render/draw.cpp
  src/render/texture.cpp
  src/render/gpu_metal.mm
  src/render/window.cpp
  src/render/controls.cpp
  src/render/menu.cpp
  src/render/settings.cpp
  src/render/overlay.mm
  src/render/font.cpp
  src/render/english_textures.cpp
  src/render/activity.mm
  src/render/audio.cpp
  "${SHADER_BUILD_DIR}/shaders.cpp")
target_include_directories(ikaruga_render PUBLIC src)
target_link_libraries(ikaruga_render PUBLIC SDL3::SDL3 "-framework Metal" "-framework QuartzCore" "-framework Foundation"
                      "-framework CoreText" "-framework CoreGraphics")
target_compile_options(ikaruga_render PRIVATE -Wall -Wextra $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# Draws a dump that IKARUGA_DUMP_TA writes, without a window, into a PNG.
find_package(ZLIB REQUIRED)
add_executable(render-view EXCLUDE_FROM_ALL tests/render_view.cpp)
target_link_libraries(render-view PRIVATE ikaruga_render ZLIB::ZLIB)

# Compares the renderer's decoding with dream-recomp's, on dumps that IKARUGA_DUMP_TA writes.
add_executable(render-compare EXCLUDE_FROM_ALL tests/render_compare.cpp)
target_link_libraries(render-compare PRIVATE ikaruga_render dream::render)

# The runtime: guest memory, the machine, the boot ROM services, the SH-4 interpreter for Ninja and
# the differential tests.
add_executable(ikaruga
  src/main.cpp
  src/memory.cpp
  src/core.cpp
  src/machine.cpp
  src/pvr.cpp
  src/bios.cpp
  src/clock.cpp
  src/disc.cpp
  src/files.cpp
  src/frame.cpp
  src/kamui.cpp
  src/kamui_texture.cpp
  src/input.cpp
  src/ninja.cpp
  src/ninja_draw.cpp
  src/ninja_chunk.cpp
  src/ninja_motion.cpp
  src/ninja_texture.cpp
  src/ninja_init.cpp
  src/saves.cpp
  src/native.cpp
  src/trace.cpp
  src/video.cpp
  src/diff.cpp
  src/game_tweaks.cpp
  src/text.cpp
  src/sh4/sh4.cpp
  src/sh4/selftest.cpp)
target_link_libraries(ikaruga PRIVATE ikaruga_decomp ikaruga_render dream::runtime)
target_compile_options(ikaruga PRIVATE -g -Wall -Wextra -Wno-unused-parameter -fwrapv -fno-strict-aliasing
                       -ffp-contract=off)

# Ikaruga.app: the same program in an app bundle, whose Info.plist declares it a game, so that macOS
# turns on Game Mode while its window is in fullscreen. Started from Finder, without options, it
# plays in a window and asks for the disc once (README).
set(IKARUGA_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/Ikaruga.app")
configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" COPYONLY)

# The app's icon, which icon/icon.cpp draws and iconutil packs.
add_executable(ikaruga-icon icon/icon.cpp)
target_link_libraries(ikaruga-icon PRIVATE "-framework CoreGraphics" "-framework ImageIO" "-framework CoreFoundation")
set(IKARUGA_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/Ikaruga.iconset")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Ikaruga.icns"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${IKARUGA_ICONSET}"
  COMMAND ikaruga-icon "${IKARUGA_ICONSET}"
  COMMAND iconutil -c icns "${IKARUGA_ICONSET}" -o "${CMAKE_CURRENT_BINARY_DIR}/Ikaruga.icns"
  DEPENDS ikaruga-icon
  VERBATIM)

add_custom_command(OUTPUT "${IKARUGA_BUNDLE}/Contents/MacOS/ikaruga"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${IKARUGA_BUNDLE}/Contents/MacOS"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${IKARUGA_BUNDLE}/Contents/Resources"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" "${IKARUGA_BUNDLE}/Contents/Info.plist"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/Ikaruga.icns" "${IKARUGA_BUNDLE}/Contents/Resources/Ikaruga.icns"
  # A new file each time: macOS kills a program whose file was written over after it ran.
  COMMAND ${CMAKE_COMMAND} -E rm -f "${IKARUGA_BUNDLE}/Contents/MacOS/ikaruga"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ikaruga>" "${IKARUGA_BUNDLE}/Contents/MacOS/ikaruga"
  # An ad-hoc signature, which seals the Info.plist with the program.
  COMMAND codesign --force --sign - "${IKARUGA_BUNDLE}"
  DEPENDS ikaruga "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" "${CMAKE_CURRENT_BINARY_DIR}/Ikaruga.icns"
  COMMENT "Ikaruga.app"
  VERBATIM)
add_custom_target(bundle ALL DEPENDS "${IKARUGA_BUNDLE}/Contents/MacOS/ikaruga")
