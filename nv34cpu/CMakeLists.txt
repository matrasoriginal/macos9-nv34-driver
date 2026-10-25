# NV34 CPU — OS 9 PPC aplikácia (Retro68): plný takt PowerBook6,2 cez GPIO KeyLargo.
cmake_minimum_required(VERSION 3.10)
project(NV34CPU C CXX)
add_application(NV34CPU main.c CONSOLE)
target_link_libraries(NV34CPU NameRegistryLib)
