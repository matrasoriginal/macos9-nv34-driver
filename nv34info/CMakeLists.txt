# NV34 Info — OS 9 PPC application (Retro68).
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.10)
project(NV34Info C CXX)
add_application(NV34Info main.c CONSOLE)
target_link_libraries(NV34Info NameRegistryLib)
