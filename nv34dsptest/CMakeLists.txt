# NV34 DSp Test — DrawSprocket (fade, mode switch) step by step with a log (Retro68 PPC).
cmake_minimum_required(VERSION 3.10)
project(NV34DSpTest C CXX)
add_application(NV34DSpTest main.c)
target_compile_options(NV34DSpTest PRIVATE -O2 -Wall -Wno-pointer-sign -Wno-multichar)
