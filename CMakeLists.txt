cmake_minimum_required(VERSION 3.16...3.30)

project(SysWatch
    VERSION 1.0.0
    DESCRIPTION "Kernel syscall telemetry with a C++23 TUI"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default build type to Release if not specified for maximum compiler optimization
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Generate compile_commands.json automatically to support LSP extensions (clangd, VSCode, Neovim)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Main Executable Target Definition
# -----------------------------------------------------------------------------
add_executable(syswatch
    main.cpp
    filters.cpp
    renderer.cpp
    view.cpp
    capture.cpp
    syscall_table.cpp
    text_util.cpp
)

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
target_sources(syswatch PRIVATE ${SYSWATCH_SYSCALL_INC})

# Track headers explicitly for asset graph changes and IDE indexing structures
target_sources(syswatch PRIVATE
    filters.hpp
    types.hpp
    view.hpp
    renderer.hpp
    mock_generator.hpp
    syscall_monitor_shared.h
    syscall_table.hpp
    text_util.hpp
)

# Ensure the source tree directory is mounted on runtime include paths
target_include_directories(syswatch PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# -----------------------------------------------------------------------------
# Compiler Diagnostics and Security Warning Toggles
# -----------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(syswatch PRIVATE
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
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(syswatch PRIVATE
        /W4
        /permissive-
        /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14640
    )
endif()

# -----------------------------------------------------------------------------
# Production Optimization Pipeline Switches (Release Settings)
# -----------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Configuring aggressive pipeline optimizations for high-throughput tracking...")
    
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(syswatch PRIVATE
            -O3                # Maximum loop optimizations and vectorizer transformations
            -march=native      # Target specific host machine instruction optimization structures directly
            -fno-plt           # Reduce library call wrapper processing delays
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        target_compile_options(syswatch PRIVATE
            /O2                # Maximize execution speed
            /Oi                # Enable compiler intrinsic operations
            /Ot                # Favor high-speed executable execution blocks
        )
    endif()

    # Enable Interprocedural Optimization / Link-Time Optimization (IPO/LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        message(STATUS "Link-Time Optimization (LTO/IPO) is supported. Enabling cross-module compiler inline maps.")
        set_target_properties(syswatch PROPERTIES 
            INTERPROCEDURAL_OPTIMIZATION TRUE
        )
    else()
        message(WARNING "Link-Time Optimization is not supported by your toolchain environment: ${ipo_error}")
    endif()
endif()

# -----------------------------------------------------------------------------
# Multithreaded Target Dependency Mapping (Compulsory for Mock Generator)
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(syswatch PRIVATE 
    Threads::Threads
)

# -----------------------------------------------------------------------------
# Distribution and System Install Configurations
# -----------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS syswatch
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
