# Cross-compilation toolchain for aarch64-linux-gnu (RK3326 / R36S)
#
# Sysroot: ~/tools/r36s-sysroot
#   - glibc 2.41 runtime + headers from Debian trixie arm64 (matches the
#     dArkOS device exactly: Debian GNU/Linux 13 "trixie", glibc 2.41-12+deb13u1)
#   - linux-libc-dev 6.1.176 kernel headers
#   - gcc 16.2.0 / binutils 2.47 from the Arch aarch64-linux-gnu packages
#
# The earlier sysroot (~/tools/aarch64-sysroot) shipped glibc 2.44, which
# produced binaries requiring GLIBC_2.43 — newer than the device's 2.41, so
# the loader refused to start them ("version `GLIBC_2.43' not found").
# This sysroot pins the baseline to what the device actually provides.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_r36s_sysroot "/home/amarqs/tools/r36s-sysroot")
set(_r36s_triplet "aarch64-linux-gnu")
set(_r36s_libdir "${_r36s_sysroot}/usr/lib/${_r36s_triplet}")

set(CMAKE_C_COMPILER "${_r36s_sysroot}/usr/bin/${_r36s_triplet}-gcc")

# The Arch gcc build hardcodes its own include/lib search relative to the
# toolchain prefix; point it at the Debian-layout sysroot instead.
set(CMAKE_SYSROOT "${_r36s_sysroot}/usr")

# crt1.o/crti.o/crtn.o and libc_nonshared.a live in the multiarch libdir.
# These must survive every cmake reconfigure, so set them as CACHE entries
# rather than the _INIT variables (which the toolchain file's cache scope
# can drop on a second configure run).
set(CMAKE_EXE_LINKER_FLAGS "-B${_r36s_libdir}/ -L${_r36s_libdir}/" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "-B${_r36s_libdir}/ -L${_r36s_libdir}/" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "-B${_r36s_libdir}/ -L${_r36s_libdir}/" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${_r36s_sysroot}/usr")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# GLES 3.2 backend: the target sysroot has no SDL2/EGL/GLES development
# packages, so the renderer is compiled out for the cross build and the
# software rasterizer is used. The device has the runtime libraries, so the
# port can be switched to GLES at deploy time via MC3_RENDER once a
# cross-built SDL2/EGL dev sysroot is available.
set(PSPRECOMP_HAVE_GLES OFF CACHE BOOL "" FORCE)
