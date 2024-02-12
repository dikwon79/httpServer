cmake_minimum_required(VERSION 3.26)
project(http C)

set(CMAKE_C_STANDARD 17)

add_executable(http main_sunday.c)
target_link_libraries(http gdbm_compat)
