# NV34 Bench — OS 9 PPC application (Retro68), software QuickDraw benchmark.
cmake_minimum_required(VERSION 3.10)
project(NV34Bench C CXX)
add_application(NV34Bench main.c)
target_link_libraries(NV34Bench NameRegistryLib)
target_compile_options(NV34Bench PRIVATE -O2)
