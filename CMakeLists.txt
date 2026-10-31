cmake_minimum_required(VERSION 3.16)
project(mc3_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# psprecomp toolkit (static lib + allegrexrecomp tool).
add_subdirectory(src/psprecomp)

# The Midnight Club: L.A. Remix host.
add_subdirectory(src/mc3)
