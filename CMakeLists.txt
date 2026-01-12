cmake_minimum_required(VERSION 3.10)

project(main)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(counter counter.c)
add_executable(counter_daughter counter_daughter.c)

if(UNIX AND NOT APPLE)
    # Для семафоров
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
    target_link_libraries(counter PRIVATE pthread rt)
    target_link_libraries(counter_daughter PRIVATE pthread rt)
endif()