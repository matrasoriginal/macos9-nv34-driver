# NV34 GL Check — diagnoses loading of OpenGLLibrary / ref_gl.lib via CFM (Retro68 PPC).
cmake_minimum_required(VERSION 3.10)
project(NV34GLCheck C CXX)
add_application(NV34GLCheck main.c)
target_compile_options(NV34GLCheck PRIVATE -O2 -Wall -Wno-pointer-sign -Wno-multichar)
