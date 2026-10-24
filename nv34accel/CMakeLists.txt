# NV34 Accel — OS 9 PPC test 2D enginu (Retro68).
cmake_minimum_required(VERSION 3.10)
project(NV34Accel C CXX)
add_application(NV34Accel main.c ../accel/nv34_accel.c)
target_link_libraries(NV34Accel NameRegistryLib)
target_compile_options(NV34Accel PRIVATE -O2 -Wall -Wno-pointer-sign -Wno-stringop-truncation)
