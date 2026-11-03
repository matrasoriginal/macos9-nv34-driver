# NV34 CPU — OS 9 PPC application (Retro68): full PowerBook6,2 CPU clock via KeyLargo GPIO.
cmake_minimum_required(VERSION 3.10)
project(NV34CPU C CXX)
add_application(NV34CPU main.c CONSOLE)
target_link_libraries(NV34CPU NameRegistryLib)
