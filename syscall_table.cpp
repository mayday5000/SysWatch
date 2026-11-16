#include "syscall_table.hpp"
#include <algorithm>

namespace {
constexpr SyscallName kSyscallNames[] = {
#include "syscall_names.inc"
};
}

std::span<const SyscallName> syscall_table() {
    return kSyscallNames;
}

std::string syscall_name(int32_t nr) {
    const auto table = syscall_table();
    auto it = std::lower_bound(table.begin(), table.end(), nr,
                               [](const SyscallName &s, int32_t v) { return s.nr < v; });
    if (it != table.end() && it->nr == nr)
        return it->name;
    return "sys_" + std::to_string(nr);
}

int32_t syscall_count() {
    const auto table = syscall_table();
    return table.empty() ? 0 : table.back().nr + 1;
}
