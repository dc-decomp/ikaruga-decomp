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
# Maple, AICA with its ARM7), its renderer and its audio output. Only the libraries the port links
# are built.
set(DREAM_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dream-recomp" CACHE PATH "The dream-recomp checkout")
set(DREAM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DREAM_DEV_INTERPRETER OFF CACHE BOOL "" FORCE)
add_subdirectory("${DREAM_RECOMP_DIR}" dream EXCLUDE_FROM_ALL)

# The runtime: guest memory, the machine, the boot ROM services, the SH-4 interpreter for Ninja and
# the differential tests.
add_executable(ikaruga
  src/main.cpp
  src/memory.cpp
  src/core.cpp
  src/machine.cpp
  src/bios.cpp
  src/clock.cpp
  src/disc.cpp
  src/files.cpp
  src/input.cpp
  src/ninja.cpp
  src/ninja_draw.cpp
  src/saves.cpp
  src/native.cpp
  src/trace.cpp
  src/diff.cpp
  src/sh4/sh4.cpp
  src/sh4/selftest.cpp)
target_link_libraries(ikaruga PRIVATE ikaruga_decomp dream::runtime dream::render_vk dream::audio)
target_compile_options(ikaruga PRIVATE -g -Wall -Wextra -Wno-unused-parameter -fwrapv -fno-strict-aliasing
                       -ffp-contract=off)
