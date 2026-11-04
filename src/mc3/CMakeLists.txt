# mc3 — host for Midnight Club: L.A. Remix (USA).
#
# The generated C (mc3_funcs_*.c, mc3_register.c, mc3_imports.c) is produced
# by `allegrexrecomp emit <EBOOT.ELF> <outdir> mc3` and lives in scratch/generated.
# Regenerate whenever the ELF or psprecomp version changes.

set(GEN_DIR "${CMAKE_SOURCE_DIR}/scratch/generated")

file(GLOB GEN_FUNCS "${GEN_DIR}/mc3_funcs_*.c")

# --- split the registration table -------------------------------------------
#
# `mc3_register.c` is a single translation unit holding ~34,000 registration
# calls and ~98,500 forward declarations. Compiling it as-is takes ~40 minutes
# and 2.1 GB RSS on its own, and it is one translation unit, so `make -j`
# cannot parallelise it. Splitting it into N parts brings each chunk down to
# ~50 seconds and lets the build run them concurrently.
#
# tools/split_register.py rewrites mc3_register.c into mc3_register_partNN.c
# plus a small dispatcher. It is idempotent: running it on an already-split
# directory is a no-op because the dispatcher no longer contains the calls.
set(REGISTER_PARTS 8)
set(SPLIT_DIR "${GEN_DIR}/split")

if(EXISTS "${GEN_DIR}/mc3_register.c")
    file(READ "${GEN_DIR}/mc3_register.c" _register_contents)
    # Only split when the file still holds the registration calls inline.
    if(_register_contents MATCHES "psp_register_label")
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            message(STATUS "mc3: splitting mc3_register.c into ${REGISTER_PARTS} parts")
            execute_process(
                COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/split_register.py"
                        "${GEN_DIR}/mc3_register.c"
                        "${SPLIT_DIR}"
                        ${REGISTER_PARTS}
                RESULT_VARIABLE _split_result
                OUTPUT_VARIABLE _split_out
                ERROR_VARIABLE  _split_err)
            if(NOT _split_result EQUAL 0)
                message(WARNING "mc3: split_register.py failed (${_split_result}): ${_split_err}")
            endif()
        else()
            message(WARNING "mc3: python3 not found — building unsplit mc3_register.c (slow)")
        endif()
    endif()
endif()

if(EXISTS "${SPLIT_DIR}/mc3_register.c")
    file(GLOB GEN_REGISTER_PARTS "${SPLIT_DIR}/mc3_register_part*.c")
    set(MC3_SRCS
        main.c
        ${SPLIT_DIR}/mc3_register.c
        ${GEN_REGISTER_PARTS}
        ${GEN_DIR}/mc3_imports.c
        ${GEN_FUNCS}
    )
    message(STATUS "mc3: registration table split into ${REGISTER_PARTS} parallel parts")
else()
    set(MC3_SRCS
        main.c
        ${GEN_DIR}/mc3_register.c
        ${GEN_DIR}/mc3_imports.c
        ${GEN_FUNCS}
    )
endif()

# Optional SDL2 render backend (works on any host with SDL2).
# Separate from GLES backends — this one uses plain SDL2 software rendering
# with no EGL/GLES dependency, so it works on any Linux host with SDL2.
set(PSPRECOMP_HAVE_SDL2 0)
if(NOT CMAKE_CROSSCOMPILING)
    execute_process(COMMAND pkg-config --exists sdl2
                RESULT_VARIABLE PKG_SDL2_OK OUTPUT_QUIET ERROR_QUIET)
    if (PKG_SDL2_OK EQUAL 0)
        execute_process(COMMAND pkg-config --libs sdl2
                        OUTPUT_VARIABLE PKG_SDL2_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND pkg-config --cflags sdl2
                        OUTPUT_VARIABLE PKG_SDL2_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(PSPRECOMP_HAVE_SDL2 1)
        list(APPEND MC3_SRCS render_sdl.c)
        message(STATUS "mc3: SDL2 render backend enabled (SDL2 via pkg-config)")
    else()
        message(WARNING "mc3: SDL2 not found — sdl2 backend unavailable")
    endif()

    # Terminal backend: always available, draws ANSI blocks in terminal.
    # Guaranteed to display something visible on any Linux terminal.
    list(APPEND MC3_SRCS render_terminal.c)
    message(STATUS "mc3: terminal render backend enabled (ANSI blocks)")
else()
    # Cross build: check target sysroot
    if(DEFINED ENV{PKG_CONFIG_SYSROOT_DIR})
        execute_process(COMMAND pkg-config --exists sdl2
                    RESULT_VARIABLE PKG_SDL2_OK OUTPUT_QUIET ERROR_QUIET)
        if (PKG_SDL2_OK EQUAL 0)
            execute_process(COMMAND pkg-config --libs sdl2
                            OUTPUT_VARIABLE PKG_SDL2_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
            execute_process(COMMAND pkg-config --cflags sdl2
                            OUTPUT_VARIABLE PKG_SDL2_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(REPLACE "$ENV{PKG_CONFIG_SYSROOT_DIR}" "" PKG_SDL2_CFLAGS "${PKG_SDL2_CFLAGS}")
            string(REGEX REPLACE "-L[^ ]+ ?" "" PKG_SDL2_LIBS "${PKG_SDL2_LIBS}")
            string(STRIP "${PKG_SDL2_LIBS}" PKG_SDL2_LIBS)
            set(PSPRECOMP_HAVE_SDL2 1)
            list(APPEND MC3_SRCS render_sdl.c)
            message(STATUS "mc3: SDL2 render backend enabled (cross, target sysroot)")
        else()
            message(WARNING "mc3: cross build — SDL2 not in target sysroot, software only")
        endif()
    else()
        message(WARNING "mc3: cross build — PKG_CONFIG_SYSROOT_DIR unset, software only")
    endif()
endif()

# Optional SDL2 + GLES 3.2 + EGL for the gles32 render backend.
#
# On a native build the host's pkg-config answers. On a cross build the target
# sysroot may carry its own .pc files (~/tools/r36s-sysroot ships sdl2.pc,
# egl.pc and glesv2.pc), so honour PKG_CONFIG_SYSROOT_DIR/PKG_CONFIG_LIBDIR
# from the environment and ask the cross pkg-config instead of giving up.
set(PSPRECOMP_HAVE_GLES 0)
if(NOT CMAKE_CROSSCOMPILING)
    execute_process(COMMAND pkg-config --exists sdl2 egl glesv2
                RESULT_VARIABLE PKG_GLES_OK OUTPUT_QUIET ERROR_QUIET)
    if (PKG_GLES_OK EQUAL 0)
        execute_process(COMMAND pkg-config --libs sdl2 egl glesv2
                        OUTPUT_VARIABLE PKG_GLES_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND pkg-config --cflags sdl2 egl glesv2
                        OUTPUT_VARIABLE PKG_GLES_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(PSPRECOMP_HAVE_GLES 1)
        list(APPEND MC3_SRCS render_gles.c render_gles2.c)
        message(STATUS "mc3: GLES 3.2 + GLES 2.0 backend enabled (SDL2/EGL/GLESv2 via pkg-config)")
    else()
        message(WARNING "mc3: SDL2/EGL/GLESv2 not found — gles32 backend unavailable, software only")
    endif()
else()
    # Cross build: the target sysroot may still provide the development
    # packages, in which case the GPU path is worth having — the software
    # rasterizer measures ~3.4 fps on the RK3326.
    if(DEFINED ENV{PKG_CONFIG_SYSROOT_DIR})
        execute_process(COMMAND pkg-config --exists sdl2 egl glesv2
                    RESULT_VARIABLE PKG_GLES_OK OUTPUT_QUIET ERROR_QUIET)
        if (PKG_GLES_OK EQUAL 0)
            execute_process(COMMAND pkg-config --libs sdl2 egl glesv2
                            OUTPUT_VARIABLE PKG_GLES_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
            execute_process(COMMAND pkg-config --cflags sdl2 egl glesv2
                            OUTPUT_VARIABLE PKG_GLES_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
            # pkg-config prefixes paths with the sysroot; strip it back off so
            # the compiler sees plain /usr/include and /usr/lib paths.
            string(REPLACE "$ENV{PKG_CONFIG_SYSROOT_DIR}" "" PKG_GLES_CFLAGS "${PKG_GLES_CFLAGS}")
            # Keep only the -l flags: the toolchain file already supplies the
            # correct -L for the target sysroot, and pkg-config's absolute
            # -L would point outside it.
            string(REGEX REPLACE "-L[^ ]+ ?" "" PKG_GLES_LIBS "${PKG_GLES_LIBS}")
            string(STRIP "${PKG_GLES_LIBS}" PKG_GLES_LIBS)
            set(PSPRECOMP_HAVE_GLES 1)
            list(APPEND MC3_SRCS render_gles.c render_gles2.c)
            message(STATUS "mc3: GLES 3.2 + GLES 2.0 backend enabled (cross, target sysroot)")
        else()
            message(WARNING "mc3: cross build — SDL2/EGL/GLESv2 not in target sysroot, software only")
        endif()
    else()
        message(WARNING "mc3: cross build — PKG_CONFIG_SYSROOT_DIR unset, software only")
    endif()
endif()

add_executable(mc3 ${MC3_SRCS})

target_include_directories(mc3 PRIVATE
    ${CMAKE_SOURCE_DIR}/src/psprecomp/include
    ${GEN_DIR}
)

target_link_libraries(mc3 PRIVATE psprecomp m)

if (PSPRECOMP_HAVE_GLES)
    target_compile_definitions(mc3 PRIVATE PSPRECOMP_HAVE_GLES=1)
    target_compile_options(mc3 PRIVATE ${PKG_GLES_CFLAGS})
    target_link_libraries(mc3 PRIVATE ${PKG_GLES_LIBS})
    # The device's EGL/GLES is the Mali blob (libMali.so): a vendor library
    # whose own undefined symbols (GLIBCXX_3.4, wl_egl_window_*, gbm_*) are
    # resolved by the device's loader, not by this link. --allow-shlib-undefined
    # is the standard way to link against such a blob; without it the linker
    # demands a provider for every symbol the blob leaves undefined.
    target_link_options(mc3 PRIVATE -Wl,--allow-shlib-undefined)
    target_link_libraries(mc3 PRIVATE stdc++)
endif()

if (PSPRECOMP_HAVE_SDL2)
    target_compile_definitions(mc3 PRIVATE PSPRECOMP_HAVE_SDL2=1)
    target_compile_options(mc3 PRIVATE ${PKG_SDL2_CFLAGS})
    target_link_libraries(mc3 PRIVATE ${PKG_SDL2_LIBS})
endif()

# Diagnostic trace: record every function entry so we can see the full boot
# chain in stderr. Slows the run down but is essential for debugging.
target_compile_definitions(mc3 PRIVATE PSPRECOMP_TRACE=1)
target_compile_definitions(mc3 PRIVATE PSPRECOMP_TRACE_SCHED=1)

# Diagnostic build: records every function entry so psp_trace_watch_multi()
# can report how far down the boot->GE chain the run actually got. Costs one
# store per call and slows the run down, so it is a separate target rather
# than a flag on the shipping binary.
add_executable(mc3_trace ${MC3_SRCS})
target_include_directories(mc3_trace PRIVATE
    ${CMAKE_SOURCE_DIR}/src/psprecomp/include
    ${GEN_DIR}
)
target_link_libraries(mc3_trace PRIVATE psprecomp m)
target_compile_definitions(mc3_trace PRIVATE PSPRECOMP_TRACE=1)
if (PSPRECOMP_HAVE_GLES)
    target_compile_definitions(mc3_trace PRIVATE PSPRECOMP_HAVE_GLES=1)
    target_compile_options(mc3_trace PRIVATE ${PKG_GLES_CFLAGS})
    target_link_libraries(mc3_trace PRIVATE ${PKG_GLES_LIBS})
    # Same vendor-library handling as mc3: EGL/GLES symbols resolved at runtime
    target_link_options(mc3_trace PRIVATE -Wl,--allow-shlib-undefined)
    target_link_libraries(mc3_trace PRIVATE stdc++)
endif()
if (PSPRECOMP_HAVE_SDL2)
    target_compile_definitions(mc3_trace PRIVATE PSPRECOMP_HAVE_SDL2=1)
    target_compile_options(mc3_trace PRIVATE ${PKG_SDL2_CFLAGS})
    target_link_libraries(mc3_trace PRIVATE ${PKG_SDL2_LIBS})
endif()

# Emit a reminder if the generated directory is empty (forgot to run `emit`).
if(NOT GEN_FUNCS)
    message(WARNING "mc3: scratch/generated/mc3_funcs_*.c is empty — run \""
        "src/psprecomp/build-host/tools/allegrexrecomp/allegrexrecomp emit "
        "<EBOOT.ELF> scratch/generated mc3\" before building mc3")
endif()
