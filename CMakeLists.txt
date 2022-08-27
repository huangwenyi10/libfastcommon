cmake_minimum_required(VERSION 3.15)

project(libfastcommon C)

set(CMAKE_CXX_STANDARD 11)

include_directories(src)
include_directories(php-fastcommon)

add_executable(libfastcommon
        src/shared_func.c
        src/ini_file_reader.c
        src/logger.c
        src/process_ctrl.c
        src/process_ctrl.h
        src/shared_func.h)