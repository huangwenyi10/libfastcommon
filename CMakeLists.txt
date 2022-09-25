cmake_minimum_required(VERSION 3.15)

project(libfastcommon C)

set(CMAKE_CXX_STANDARD 11)

include_directories(src)


add_executable(libfastcommon
        src/array_allocator.c
        src/array_allocator.h
        src/avl_tree.c
        src/avl_tree.h
        src/base64.c
        src/base64.h
        src/buffered_file_writer.c
        src/buffered_file_writer.h
        src/chain.c
        src/chain.h
        src/char_convert_loader.c
        src/char_convert_loader.h
        src/char_converter.c
        src/char_converter.h
        src/process_ctrl.c
        src/process_ctrl.h
        src/ioevent.c
        src/ioevent.h
        )
