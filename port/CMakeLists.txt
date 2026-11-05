# Native port build: Ikaruga's decompiled code on a native runtime, without dream-recomp.
#
#   cmake -S port -B build-port -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-port
#
# The decompiled code is generated from the user's disc by tools/ghidra/analyze.sh and
# tools/decomp/convert.py into decomp/ (IKARUGA_DECOMP_DIR), which stays out of this repository.
#
# On macOS the renderer uses Metal; on Linux and Windows it uses Vulkan (port/src/render/gpu_vulkan.cpp).
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
if(MSVC)
  target_compile_options(ikaruga_decomp PRIVATE /W0 /O1 /fp:precise)
else()
  target_compile_options(ikaruga_decomp PRIVATE -fwrapv -fno-strict-aliasing -w -O1 -g -ffp-contract=off)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(ikaruga_decomp PRIVATE -ferror-limit=0)
  endif()
endif()

# The console: dream-recomp's runtime (memory map, scheduler, interrupt controllers, PowerVR, DMA,
# Maple, AICA with its ARM7). Only the libraries the port links are built.
set(DREAM_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dream-recomp" CACHE PATH "The dream-recomp checkout")
set(DREAM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DREAM_DEV_INTERPRETER OFF CACHE BOOL "" FORCE)
add_subdirectory("${DREAM_RECOMP_DIR}" dream EXCLUDE_FROM_ALL)

find_package(SDL3 CONFIG REQUIRED)

# The graphical launcher's immediate-mode UI. Vendored so the launcher needs only SDL3 at run time.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(ikaruga_imgui STATIC
  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(ikaruga_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(ikaruga_imgui PUBLIC SDL3::SDL3)
if(MSVC)
  target_compile_options(ikaruga_imgui PRIVATE /W0)
else()
  target_compile_options(ikaruga_imgui PRIVATE -w)
endif()

add_library(ikaruga_render STATIC
  src/render/ta.cpp
  src/render/draw.cpp
  src/render/texture.cpp
  src/render/window.cpp
  src/render/controls.cpp
  src/render/menu.cpp
  src/render/settings.cpp
  src/render/english_textures.cpp
  src/render/audio.cpp)

if(APPLE)
  enable_language(OBJCXX)
  target_sources(ikaruga_render PRIVATE
    src/render/gpu_metal.mm
    src/render/overlay.mm
    src/render/font.cpp
    src/render/activity.mm)
  set(SHADER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  set(SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/geometry.metal")
  add_custom_command(OUTPUT "${SHADER_BUILD_DIR}/shaders.cpp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_BUILD_DIR}"
    COMMAND ${CMAKE_COMMAND} "-DOUTPUT=${SHADER_BUILD_DIR}/shaders.cpp" "-DINPUTS=${SHADER_SOURCE}=geometry_metal"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS "${SHADER_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake" VERBATIM)
  target_sources(ikaruga_render PRIVATE "${SHADER_BUILD_DIR}/shaders.cpp")
  target_link_libraries(ikaruga_render PUBLIC SDL3::SDL3
    "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreText"
    "-framework CoreGraphics")
  target_compile_options(ikaruga_render PRIVATE -Wall -Wextra $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
  find_package(Vulkan REQUIRED)
  find_program(GLSLC NAMES glslc REQUIRED)
  target_sources(ikaruga_render PRIVATE
    src/render/gpu_vulkan.cpp
    src/render/stb_text.cpp
    src/render/activity_stub.cpp)
  set(SHADER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  set(SHADER_HEADERS)
  set(SHADER_NAMES geometry.vert geometry.frag translucent.frag added.frag added_top.frag sort.frag
                   present.vert present.frag bloom_extract.frag bloom_blur.frag bloom_composite.frag)
  foreach(shader IN LISTS SHADER_NAMES)
    string(REPLACE "." "_" symbol "${shader}")
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader}")
    set(words "${SHADER_BUILD_DIR}/${symbol}.c")
    set(header "${SHADER_BUILD_DIR}/${symbol}.h")
    add_custom_command(OUTPUT "${header}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_BUILD_DIR}"
      COMMAND "${GLSLC}" --target-env=vulkan1.1 -O -mfmt=c "${source}" -o "${words}"
      COMMAND ${CMAKE_COMMAND} -DIN=${words} -DOUT=${header} -DSYM=${symbol}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake
      DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake" VERBATIM)
    list(APPEND SHADER_HEADERS "${header}")
  endforeach()
  target_sources(ikaruga_render PRIVATE ${SHADER_HEADERS})
  target_include_directories(ikaruga_render PRIVATE "${SHADER_BUILD_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_link_libraries(ikaruga_render PUBLIC SDL3::SDL3 Vulkan::Vulkan)
  target_compile_options(ikaruga_render PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

target_include_directories(ikaruga_render PUBLIC src third_party)

# Draws a dump that IKARUGA_DUMP_TA writes, without a window, into a PNG (UNIX only: it uses zlib).
if(UNIX)
  find_package(ZLIB REQUIRED)
  add_executable(render-view EXCLUDE_FROM_ALL tests/render_view.cpp)
  target_link_libraries(render-view PRIVATE ikaruga_render ZLIB::ZLIB)
endif()

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
if(MSVC)
  target_compile_options(ikaruga PRIVATE /W3 /fp:precise)
else()
  target_compile_options(ikaruga PRIVATE -g -Wall -Wextra -Wno-unused-parameter -fwrapv -fno-strict-aliasing
                         -ffp-contract=off)
endif()
if(WIN32)
  # A double-clicked build has no PATH set up for the DLL, so keep it next to the program.
  add_custom_command(TARGET ikaruga POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:ikaruga>"
    VERBATIM)
endif()
if(MINGW)
  # No MinGW runtime DLLs to ship beside the game.
  target_link_options(ikaruga PRIVATE -static-libgcc -static-libstdc++)
endif()

# The launcher: accepts a GDI or a CHD, unpacks the CHD to a GDI, extracts the disc's files, and
# starts the game. It uses libchdr, the same decoder the dcdisc tooling uses (dream-recomp already
# builds it as chdr-static).
add_executable(ikaruga-launcher src/launcher.cpp src/launcher_gui.cpp)
target_include_directories(ikaruga-launcher PRIVATE
  "${DREAM_RECOMP_DIR}/third_party/libchdr/include")
target_link_libraries(ikaruga-launcher PRIVATE chdr-static ikaruga_imgui SDL3::SDL3)
if(NOT MSVC)
  target_compile_options(ikaruga-launcher PRIVATE -Wall -Wextra)
endif()
if(MINGW)
  target_link_options(ikaruga-launcher PRIVATE -static-libgcc -static-libstdc++)
endif()
if(WIN32)
  # The launcher is a windowed program; launcher.cpp supplies WinMain and attaches a console only
  # when it is invoked with command-line arguments. Keep SDL3.dll beside both programs.
  set_target_properties(ikaruga-launcher PROPERTIES WIN32_EXECUTABLE TRUE)
  add_custom_command(TARGET ikaruga-launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:ikaruga-launcher>"
    VERBATIM)
endif()

# Ikaruga.app: the same program in an app bundle, whose Info.plist declares it a game, so that macOS
# turns on Game Mode while its window is in fullscreen. Started from Finder, without options, it
# plays in a window and asks for the disc once (README).
if(APPLE)
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
endif()

# Linux AppImage: the GUI launcher and game together, with SDL3 and an AppRun wrapper.
if(UNIX AND NOT APPLE)
  add_custom_target(appimage
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../tools/linux/make_appimage.sh" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS ikaruga ikaruga-launcher
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    COMMENT "Building Ikaruga AppImage"
    VERBATIM)
endif()
