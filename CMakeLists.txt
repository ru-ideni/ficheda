cmake_minimum_required(VERSION 3.21)
project(ficheda C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "-pthread")

add_executable(ficheda main.c)
