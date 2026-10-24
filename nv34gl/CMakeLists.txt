# NV34 GL Test — prvý 3D trojuholník (Retro68 PPC).
cmake_minimum_required(VERSION 3.10)
project(NV34GL C CXX)
add_application(NV34GL main.c ../accel/nv34_accel.c ../accel/nv34_3d.c)
target_link_libraries(NV34GL NameRegistryLib)
target_compile_options(NV34GL PRIVATE -O2 -Wall -Wno-pointer-sign -Wno-stringop-truncation)
