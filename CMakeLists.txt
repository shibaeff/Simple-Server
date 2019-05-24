cmake_minimum_required(VERSION 2.8) # Проверка версии CMake.
SET(CMAKE_C_COMPILER /usr/bin/gcc)
add_executable(server server.c)