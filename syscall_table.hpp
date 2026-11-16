#pragma once
/*
 * syscall_table.hpp
 * Purpose: syscall number -> name for the architecture the TUI is built
 * for. The table is generated at build time from <sys/syscall.h>
 * (cmake/gen_syscall_names.cmake) and sorted by number.
 */
#include <cstdint>
#include <span>
#include <string>

struct SyscallName {
    int32_t nr;
    const char *name;
};

// All known syscalls, sorted by number.
std::span<const SyscallName> syscall_table();

// "openat" for a known number, "sys_<nr>" otherwise.
std::string syscall_name(int32_t nr);

// Highest known syscall number plus one (0 if the table is empty).
int32_t syscall_count();
