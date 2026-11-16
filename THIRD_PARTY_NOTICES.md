# Third-Party Notices

SysWatch is licensed under the MIT License (see [LICENSE](LICENSE)). It does not bundle or vendor any third-party source code. It builds against the following external components, which are not distributed in this repository:

| Component | Used for | License |
|-----------|----------|---------|
| Linux kernel headers (`linux/*`, `asm/*`) | Building the `syscall_monitor` kernel module | GPL-2.0-only |
| Linux UAPI headers (`linux/ioctl.h`, `linux/types.h`, `asm/unistd.h`) | Shared ioctl and syscall definitions used by the user-space TUI | GPL-2.0-only WITH Linux-syscall-note |
| C/C++ standard library and POSIX headers (glibc, libstdc++ or libc++) | User-space TUI dashboard | LGPL-2.1+ / GPL-3.0 with GCC Runtime Library Exception, or Apache-2.0 with LLVM exception |
| CMake, GNU Make, GCC/Clang | Build tooling only | Their respective licenses |

## Kernel module licensing

The kernel module declares `MODULE_LICENSE("Dual MIT/GPL")`. Its source is offered under the MIT License, and the dual declaration keeps it GPL-compatible so the Linux kernel allows it to register `sys_enter` tracepoint probes, which are exported to GPL-compatible modules only. When loaded into a Linux kernel, the module is combined with GPL-2.0 kernel code, and the GPL-2.0 terms apply to that combined work.
