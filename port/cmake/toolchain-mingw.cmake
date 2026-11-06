# Cross-compiling the Windows port from Linux with MinGW-w64:
#   cmake -S port -B build-win -DCMAKE_TOOLCHAIN_FILE=port/cmake/toolchain-mingw.cmake \
#         -DCMAKE_PREFIX_PATH=$PWD/.deps/sdl3-mingw-install \
#         -DVULKAN_INCLUDE_DIR=/usr/include -DVULKAN_LIBRARY=$PWD/.deps/win/vulkan/libvulkan-1.a
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
