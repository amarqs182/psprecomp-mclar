# mc3 — host for Midnight Club: L.A. Remix (USA).
#
# The generated C (mc3_funcs_*.c, mc3_register.c, mc3_imports.c) is produced
# by `allegrexrecomp emit <EBOOT.ELF> <outdir> mc3` and lives in scratch/generated.
# Regenerate whenever the ELF or psprecomp version changes.

set(GEN_DIR "${CMAKE_SOURCE_DIR}/scratch/generated")

file(GLOB GEN_FUNCS "${GEN_DIR}/mc3_funcs_*.c")
set(MC3_SRCS
    main.c
    ${GEN_DIR}/mc3_register.c
    ${GEN_DIR}/mc3_imports.c
    ${GEN_FUNCS}
)

# Optional SDL2 + GLES 3.2 + EGL for the gles32 render backend.
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
        list(APPEND MC3_SRCS render_gles.c)
        message(STATUS "mc3: GLES 3.2 backend enabled (SDL2/EGL/GLESv2 via pkg-config)")
    else()
        message(WARNING "mc3: SDL2/EGL/GLESv2 not found — gles32 backend unavailable, software only")
    endif()
else()
    message(STATUS "mc3: Cross-compiling — GLES 3.2 backend disabled, software only")
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
endif()

# Emit a reminder if the generated directory is empty (forgot to run `emit`).
if(NOT GEN_FUNCS)
    message(WARNING "mc3: scratch/generated/mc3_funcs_*.c is empty — run \""
        "src/psprecomp/build-host/tools/allegrexrecomp/allegrexrecomp emit "
        "<EBOOT.ELF> scratch/generated mc3\" before building mc3")
endif()
