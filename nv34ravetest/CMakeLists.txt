# NV34 RAVE Test — tests the NV34 RAVE engine through the RAVE API (Retro68 PPC).
cmake_minimum_required(VERSION 3.10)
project(NV34RAVETest C CXX)
add_application(NV34RAVETest main.c)
target_link_libraries(NV34RAVETest QuickDraw3DRAVELib NameRegistryLib m)
target_compile_options(NV34RAVETest PRIVATE -O2 -Wall -Wno-pointer-sign -Wno-stringop-truncation -Wno-multichar)
