cmake_minimum_required(VERSION 3.20)
project(model3recomp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# SDL2 is needed for the platform layer and the example, but not for the
# semantic tests -- so a machine without it can still run the harness.
find_package(SDL2 QUIET)

set(M3_SOURCES
    src/model3recomp.c
    src/ppc.c
    src/bus.c
    src/func_table.c
    src/irq.c
    src/scsi.c
    src/pci.c
    src/screenshot.c
    src/tilegen.c
)
if(SDL2_FOUND)
    list(APPEND M3_SOURCES src/platform_sdl.c)
else()
    # Headless build: the library is still fully usable for the boot harness
    # and the conformance runs, just without a window.
    list(APPEND M3_SOURCES src/platform_null.c)
endif()

add_library(model3recomp STATIC ${M3_SOURCES})

target_include_directories(model3recomp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(SDL2_FOUND)
    target_link_libraries(model3recomp PUBLIC SDL2::SDL2)
else()
    message(STATUS "SDL2 not found -- building headless (src/platform_null.c)")
endif()

if(NOT MSVC)
    target_link_libraries(model3recomp PUBLIC m)
    target_compile_options(model3recomp PRIVATE -Wall -Wextra -Wno-unused-parameter)
else()
    target_compile_options(model3recomp PRIVATE /W3)
    target_compile_definitions(model3recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# --- Tests ---
option(MODEL3RECOMP_BUILD_TESTS "Build test programs" ON)
if(MODEL3RECOMP_BUILD_TESTS)
    enable_testing()
    add_executable(model3recomp_test_ppc tests/test_ppc.c)
    target_link_libraries(model3recomp_test_ppc PRIVATE model3recomp)
    add_test(NAME ppc_semantics COMMAND model3recomp_test_ppc)
endif()
