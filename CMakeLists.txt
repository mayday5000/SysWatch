cmake_minimum_required(VERSION 3.20...3.31)

project(SysWatch
    VERSION 1.2.0
    DESCRIPTION "Kernel syscall telemetry with a C++23 TUI"
    LANGUAGES CXX
)

# C++23 (std::expected, std::to_underlying). CMAKE_CXX_STANDARD 23 needs
# CMake 3.20 or newer.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The project uses no C++20 modules. Without this, CMake 3.28+ with Ninja
# and Clang requires clang-scan-deps, which many distributions ship in a
# separate package.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(SYSWATCH_NATIVE "Tune Release builds for this machine's CPU (-march=native); the binary may not run elsewhere" OFF)

# Default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Generate compile_commands.json for clangd and other LSP tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Compiler diagnostics, shared by every target
# -----------------------------------------------------------------------------
add_library(syswatch_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(syswatch_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )
endif()

# -----------------------------------------------------------------------------
# Syscall number -> name table, generated for the target architecture from
# <sys/syscall.h> with the same compiler and flags as the TUI.
# -----------------------------------------------------------------------------
set(SYSWATCH_SYSCALL_INC ${CMAKE_CURRENT_BINARY_DIR}/generated/syscall_names.inc)
separate_arguments(syswatch_gen_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
if(CMAKE_CXX_COMPILER_TARGET)
    list(APPEND syswatch_gen_flags "--target=${CMAKE_CXX_COMPILER_TARGET}")
endif()
if(CMAKE_SYSROOT)
    list(APPEND syswatch_gen_flags "--sysroot=${CMAKE_SYSROOT}")
endif()
add_custom_command(
    OUTPUT ${SYSWATCH_SYSCALL_INC}
    COMMAND ${CMAKE_COMMAND}
            -DCXX=${CMAKE_CXX_COMPILER}
            "-DCXX_FLAGS=${syswatch_gen_flags}"
            -DOUT=${SYSWATCH_SYSCALL_INC}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_syscall_names.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_syscall_names.cmake
    COMMENT "Generating syscall name table"
    VERBATIM
)

# -----------------------------------------------------------------------------
# Core objects: everything except the terminal/event loop in main.cpp.
# Linked into the TUI and the unit test. An OBJECT library (not STATIC), so
# LTO bitcode never ends up in an archive the system linker cannot read.
# -----------------------------------------------------------------------------
add_library(syswatch_core OBJECT
    filters.cpp
    renderer.cpp
    view.cpp
    capture.cpp
    syscall_table.cpp
    text_util.cpp
    ${SYSWATCH_SYSCALL_INC}
)
target_sources(syswatch_core PRIVATE
    filters.hpp
    types.hpp
    view.hpp
    renderer.hpp
    capture.hpp
    decode.hpp
    syscall_monitor_shared.h
    syscall_table.hpp
    text_util.hpp
)
target_include_directories(syswatch_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(syswatch_core PRIVATE syswatch_warnings)
# One version string for the TUI title and --help; keep it in step with
# MODULE_VERSION in syscall_monitor.c.
target_compile_definitions(syswatch_core PUBLIC
    SYSWATCH_VERSION="${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")

# -----------------------------------------------------------------------------
# TUI executable
# -----------------------------------------------------------------------------
add_executable(syswatch main.cpp mock_generator.hpp)
target_link_libraries(syswatch PRIVATE syswatch_core syswatch_warnings Threads::Threads)

# -----------------------------------------------------------------------------
# Release tuning. CMake already adds -O3 -DNDEBUG for Release.
# -----------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(syswatch PRIVATE -fno-plt)
        if(SYSWATCH_NATIVE)
            target_compile_options(syswatch_core PRIVATE -march=native)
            target_compile_options(syswatch PRIVATE -march=native)
        endif()
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        message(STATUS "Link-time optimization enabled.")
        # Every target that links the core objects must use LTO too.
        set(SYSWATCH_IPO TRUE)
        set_target_properties(syswatch syswatch_core PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "Link-time optimization not supported: ${ipo_error}")
    endif()
endif()

# -----------------------------------------------------------------------------
# Tests (ctest): ABI, decoder, filters, sanitizer, syscall table, view
# -----------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(mock_test mock_test.cpp)
    target_link_libraries(mock_test PRIVATE syswatch_core syswatch_warnings)
    if(SYSWATCH_IPO)
        set_target_properties(mock_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    add_test(NAME mock_test COMMAND mock_test)
endif()

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS syswatch
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
