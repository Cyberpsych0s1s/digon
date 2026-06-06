cmake_minimum_required(VERSION 3.20)
# C is enabled so LLVM's own CMake config can run its check_c_source_compiles
# probes (FindFFI, etc). digon itself is pure C++17.
project(digon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Spec §20.2: no exceptions, no RTTI, warnings on
add_compile_options(-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)

# Spec §20.2: LLVM major version is pinned. Update both the digit below
# and the matching line in README.md when intentionally bumping.
set(DIGON_REQUIRED_LLVM_MAJOR 21)

# LLVM's own ConfigVersion file requires an exact major.minor match,
# which is finer-grained than the policy we want. We accept any patch
# of the pinned major and validate the major ourselves below.
find_package(LLVM REQUIRED CONFIG)
find_package(LLD  REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using LLDConfig.cmake  in: ${LLD_DIR}")

if(NOT LLVM_VERSION_MAJOR EQUAL DIGON_REQUIRED_LLVM_MAJOR)
    message(FATAL_ERROR
        "digon stage 0 requires LLVM ${DIGON_REQUIRED_LLVM_MAJOR}.x; found ${LLVM_PACKAGE_VERSION}. "
        "Update DIGON_REQUIRED_LLVM_MAJOR in CMakeLists.txt and the version line in README.md "
        "if you intend to switch LLVM major.")
endif()

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_compile_definitions(${LLVM_DEFINITIONS_LIST})

# Source layout (spec §20.1, reorganised by layer):
#   src/base/    arena, vec, map, intern, diag       — foundation utilities
#   src/front/   lex, ast, parse, types, fmt         — source -> AST -> typed AST
#   src/ir/      lower                               — AST -> mid-IR
#   src/back/    llvm_emit, link                     — mid-IR -> object -> exe
#   src/driver/  cli, build, main                    — top-level orchestration
# Spec §20.2: All LLVM contact is confined to back/llvm_emit and back/link.
# Includes use bare names (e.g. `#include "lex.h"`); each layer is added to
# the search path so the layered move doesn't ripple into every source file.
set(DIGON_INCLUDE_DIRS
    src/base
    src/front
    src/ir
    src/back
    src/driver
)

set(DIGON_FRONT_END_SOURCES
    src/base/arena.cc
    src/base/vec.cc
    src/base/map.cc
    src/base/intern.cc
    src/base/diag.cc
    src/front/lex.cc
    src/front/ast.cc
    src/front/parse.cc
    src/front/types.cc
    src/front/fmt.cc
    src/ir/lower.cc
    src/driver/cli.cc
    src/driver/build.cc
)

set(DIGON_BACKEND_SOURCES
    src/back/llvm_emit.cc
    src/back/link.cc
)

add_library(digon_frontend STATIC ${DIGON_FRONT_END_SOURCES})
target_include_directories(digon_frontend PUBLIC ${DIGON_INCLUDE_DIRS})

add_library(digon_backend STATIC ${DIGON_BACKEND_SOURCES})
target_include_directories(digon_backend PUBLIC ${DIGON_INCLUDE_DIRS})
target_include_directories(digon_backend SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})

# Link against libLLVM components actually used by codegen.
# Add more as llvm_emit grows (e.g. Passes, ipo, instcombine, scalaropts).
llvm_map_components_to_libnames(DIGON_LLVM_LIBS
    core
    support
    irreader
    bitwriter
    target
    x86codegen
    x86asmparser
    x86desc
    x86info
    aarch64codegen
    aarch64asmparser
    aarch64desc
    aarch64info
)

# LLD libraries — one per object format. We invoke LLD as a library so
# the toolchain stays a single binary (spec §16).
set(DIGON_LLD_LIBS
    lldCommon
    lldELF
    lldCOFF
    lldMachO
    lldWasm
)

target_link_libraries(digon_backend PUBLIC ${DIGON_LLVM_LIBS} ${DIGON_LLD_LIBS})

add_executable(digon src/driver/main.cc)
target_link_libraries(digon PRIVATE digon_frontend digon_backend)

# Tests — shell-driven per spec §20.1, plus standalone C++ test binaries
# for unit-level pieces. No test framework.
enable_testing()

foreach(unit arena vec map intern diag lex parse fmt)
    add_executable(${unit}_test tests/${unit}_test.cc)
    target_link_libraries(${unit}_test PRIVATE digon_frontend)
    add_test(NAME ${unit}_test COMMAND ${unit}_test)
endforeach()

# End-to-end golden + compile-fail tests: compile and run real .dg programs.
# Requires bash and a C compiler driver (cc) on PATH.
find_program(BASH_EXECUTABLE NAMES bash)
if(BASH_EXECUTABLE)
    add_test(NAME golden
             COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh
                     $<TARGET_FILE:digon>)
endif()
