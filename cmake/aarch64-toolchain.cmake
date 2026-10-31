# Cross-compilation toolchain for aarch64-linux-gnu (RK3326 / R36S)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER /home/amarqs/tools/aarch64-sysroot/usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_C_COMPILER_ARG1 "")

set(CMAKE_FIND_ROOT_PATH /home/amarqs/tools/aarch64-sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Disable GLES for cross build (target sysroot has no GLES libs)
set(PSPRECOMP_HAVE_GLES OFF CACHE BOOL "" FORCE)
