# QEMU tests for the kernel module

These scripts test `syscall_monitor.ko` inside a throwaway VM, so nothing is
loaded into the host kernel and no root is needed. `run.sh`:

1. builds the module against a kernel tree (`make M=... W=1`),
2. builds static test programs (`kmod_test`, `kmod_test32`, `ptyrun`, `noise`)
   and a static `syswatch`,
3. packs them with a static busybox into an initramfs,
4. boots the kernel in QEMU and runs `init` as PID 1,
5. reads the serial log and prints a verdict.

## Prerequisites

Debian/Ubuntu package names:

```bash
sudo apt install build-essential gcc-multilib cmake bc bison flex libelf-dev \
    libssl-dev dwarves cpio busybox-static qemu-system-x86
```

- `gcc-multilib` is optional; without it the 32-bit test is skipped.
- A C++23 compiler and CMake 3.20+ for the TUI (see the main README), or set
  `SKIP_TUI=1`.
- x86_64 host and kernel. KVM is not needed (the default is TCG emulation).

## Build a debug kernel

Any kernel from 5.4 on works. A debug kernel is the useful one: `debug.config`
turns on KASAN, lockdep (`PROVE_LOCKING`), `DEBUG_ATOMIC_SLEEP`, UBSAN,
`DEBUG_LIST`, syscall tracepoints (`FTRACE_SYSCALLS`), 32-bit emulation
(`IA32_EMULATION`) and a preemptible kernel, on top of `x86_64_defconfig`.

```bash
curl -LO https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.117.tar.xz
tar xf linux-6.12.117.tar.xz
KERNEL_SRC=$PWD/linux-6.12.117 tests/qemu/build-kernel.sh
```

This takes 5 to 15 minutes on 8 cores. For a `PREEMPT_RT` kernel (6.12+), add
the RT fragment:

```bash
KCONFIG_FRAGMENTS="debug.config rt.config" KERNEL_SRC=... tests/qemu/build-kernel.sh
```

## Run

```bash
# a kernel you built (module built against the same tree)
KERNEL_SRC=/path/to/linux-6.12.117 tests/qemu/run.sh

# a distribution kernel and its headers
BZIMAGE=/boot/vmlinuz-$(uname -r) KDIR=/lib/modules/$(uname -r)/build tests/qemu/run.sh
```

| Variable | Default | Meaning |
|---|---|---|
| `KERNEL_SRC` | | built kernel tree; default for `KDIR` and `BZIMAGE` |
| `KDIR` | `$KERNEL_SRC`, else `/lib/modules/$(uname -r)/build` | tree to build the module against |
| `BZIMAGE` | `$KERNEL_SRC/arch/x86/boot/bzImage` | kernel to boot |
| `KO` | | prebuilt `syscall_monitor.ko` to use instead |
| `BUSYBOX` | `busybox` on `PATH` | must be statically linked |
| `OUT` | `tests/qemu/out` | work directory (ignored by git) |
| `ACCEL` | `tcg` | `kvm` if `/dev/kvm` is usable |
| `VMTIMEOUT` | `600` | seconds before the VM is killed |
| `QEMU_MEM`, `QEMU_SMP`, `APPEND` | `2048`, `4`, empty | VM memory, CPUs, extra kernel arguments |
| `SKIP_TUI`, `SKIP_32` | `0` | `1` skips the TUI or the 32-bit test |

The module and the kernel must match: boot the image of the same tree you
pass as `KDIR`.

## What is checked

- `kmod_test`: the device node is root-only `0600`; an unprivileged open gets
  `EACCES`, and `EPERM` even through a world-readable node; ioctls and the PID
  and syscall filters; `openat` path decoding; records carry the process ID
  (also for a second thread); `read` returns `ENOBUFS` for a short buffer,
  `EAGAIN` when drained, and a blocking read is interrupted instead of
  returning 0; an idle `poll` loop does not wake itself; a lapped reader gets
  at most one ring of records; 8 threads adding and deleting syscall filters
  while 2 threads read and 2 threads make syscalls on the same descriptor;
  two readers both see the same record; unknown ioctl gives `ENOTTY`; `lseek`
  gives `ESPIPE`.
- `kmod_test32` (32-bit): the ioctls work through `compat_ioctl`; records of
  the 32-bit child carry `SM_NR_COMPAT` and are not decoded.
- `ptyrun`: `syswatch` runs on a pty against the real device (no simulator),
  shows real process and syscall names and the decoded `/proc/version` path
  opened by `noise`, hides its own syscalls, writes a capture file, and `q`
  exits with status 0 and restores the terminal.
- `rmmod` is refused while the device is open and works after it is closed.
- The kernel log has no `BUG:`, `WARNING:`, `KASAN:`, `UBSAN:`, lockdep,
  "sleeping function called from invalid context", "scheduling while atomic",
  `Oops` or panic lines.

## Pass and fail

The in-VM output is printed first (`ok` / `FAIL` per check), then a summary:

```text
PASS  module loaded
PASS  device node is root-only 0600
PASS  kmod_test
PASS  kmod_test32 (compat)
PASS  TUI on a pty
PASS  rmmod refused while open
PASS  rmmod
PASS  VM reached the end
PASS  kernel log clean (no BUG/WARNING/KASAN/UBSAN/lockdep reports)

QEMU TESTS PASSED
```

`run.sh` exits 0 when everything passed, 1 when a check failed (the offending
kernel log lines are printed), and 2 for a setup problem such as a missing
tool or kernel image. The full serial log is `tests/qemu/out/serial.log`.

## Files

| File | Purpose |
|---|---|
| `run.sh` | build everything, boot, verdict |
| `build-kernel.sh` | configure and build a debug kernel |
| `debug.config` | kernel config fragment (debug options) |
| `rt.config` | optional `PREEMPT_RT` fragment |
| `init` | PID 1 inside the VM |
| `kmod_test.c` | device tests (64-bit) |
| `kmod_test32.c` | compat tests (32-bit) |
| `ptyrun.c` | drives the TUI on a pty and checks the screen |
| `noise.c` | background process that opens `/proc/version` |
