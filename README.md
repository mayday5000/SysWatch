# SysWatch

A high performance Linux kernel syscall telemetry engine and real-time C++23 TUI dashboard driven by atomic `sys_enter` tracepoint hooks.

Engineered around a **zero-allocation hot path** across the kernel, the system intercepts live `sys_enter` tracepoint events inside a non-sleeping atomic context, copies path arguments with page faults disabled, streams fixed-width **176-byte binary records** through a **4096-slot broadcast ring** to readers of `/dev/syscall_monitor`, and renders flicker-free real-time diagnostics at high event rates without inducing desktop or terminal sluggishness.

---

## ⚡ Zero-Allocation Hot Path Architecture

### What does "Zero-Allocation" mean?

Zero-allocation means that once the program finishes starting up, no dynamic heap memory is ever requested or freed (`kmalloc`/`kfree` in the kernel) while processing live syscall events.

Dynamic memory allocation is dangerous on a hot path for three reasons:

- **Unpredictable Latency (Jitter):** A heap allocator (`kmalloc`) sometimes has to search free lists, acquire global locks, or ask the OS for new memory pages, causing sudden micro-stutters.
- **Illegal in Kernel Atomic Context:** The probe is written so it never sleeps. A standard kernel allocation (`kmalloc(..., GFP_KERNEL)`) can sleep when memory is low; in atomic context that is a bug (`BUG: sleeping function called from invalid context`, or `scheduling while atomic`), and it can hang or crash the machine.
- **Memory Fragmentation:** Allocating and freeing millions of tiny event structs per minute fragments memory and degrades CPU cache locality.

### How the Project Achieves It on the Kernel Side

| Layer | Cold Path (Allocates Once at Startup) | Hot Path (Zero Allocations per Event/Frame) |
| :--- | :--- | :--- |
| **Linux Kernel (`syscall_monitor.ko`)** | `sm_open()` calls `kzalloc()` and `kvmalloc_array()` **once** per open of `/dev/syscall_monitor` to set up `priv` and `priv->snap`. | `probe_sys_enter()` builds the record in a `struct sm_event` on its stack and copies it into the pre-allocated static ring `mon_ctx.ring[4096]`. **Zero `kmalloc` calls per syscall.** |

---

## 🎯 Purpose & Why It Exists

Traditional user-space tracers such as `strace` rely on `ptrace`. That stops the target twice per syscall, forces a context switch on every entry and exit, and can only follow one process tree. Audit frameworks avoid the stop, but they add a daemon, a netlink socket, and a text record format.

SysWatch puts a short tracepoint probe in the kernel and streams compact binary records to an interactive C++23 terminal:

- **System-wide visibility.** Every process is visible. Nothing is stopped.
- **Several readers.** Each `open` of `/dev/syscall_monitor` has its own cursor and its own `ioctl` filters. Two TUIs, or a TUI and a logger, do not share a read position.
- **Live triage.** Pause, sort, scroll, hide rows with `filters.xml`, push a PID filter into the kernel (`-p`), and write a capture file.

Scope is `sys_enter` only. There is no `sys_exit`, and argument decoding covers the path of `openat` and `execve` plus a tag for `mmap`, for native (64-bit) syscalls. Syscalls of 32-bit tasks on a 64-bit kernel are recorded with a compat flag and not decoded. The kernel module source is `GPL-2.0 OR MIT` (SPDX) and declares `MODULE_LICENSE("Dual MIT/GPL")`, which lets it use the GPL-only tracepoint API. The userspace sources are MIT.

---

## 🏗️ End-to-End System Architecture

```text
+---------------------------------------------------------------------------------------------------+
|                                          LINUX KERNEL SPACE                                       |
|                                                                                                   |
|   [ Any User Process ] ---> syscall() ---> [ Tracepoint: sys_enter ]                              |
|                                                    |                                              |
|                                                    v                                              |
|                                          +-------------------+                                    |
|                                          |  probe_sys_enter  |  (Atomic / Non-Sleeping Context)   |
|                                          |  - Read pt_regs   |                                    |
|                                          |  - Safe user copy |  pagefault_disable()               |
|                                          +---------+---------+                                    |
|                                                    | raw_spin_lock_irqsave(&mon_ctx.lock)         |
|                                                    v                                              |
|   +-------------------------------------------------------------------------------------------+   |
|   |  mon_ctx.ring[4096]  (Power-of-2 Broadcast Ring Buffer, 176 B / slot)                     |   |
|   |  [ Slot 0 ] [ Slot 1 ] ... [ seq & MASK ] ... [ Slot 4095 ]   (mon_ctx.seq++)             |   |
|   +----+-------------------------------+------------------------------------------------------+   |
|        | wake_up_interruptible(&waitq), only if a reader sleeps                                   |
|        v                               v                                                          |
|   +-----------------------+       +-----------------------+                                       |
|   |  FD #1 (sm_priv)      |       |  FD #2 (sm_priv)      |   Per-Reader State:                   |
|   |  - next_seq           |       |  - next_seq           |   - Independent ring cursor           |
|   |  - mutex, snap[]      |       |  - mutex, snap[]      |   - Dedicated snap[] staging buffer   |
|   |  - filter_pid / sys[] |       |  - filter_pid / sys[] |   - Kernel-side event predicate       |
|   +-----------+-----------+       +-----------+-----------+                                       |
+---------------|-------------------------------|---------------------------------------------------+
                |  read() -> copy_to_user()     |  ioctl() -> SET/ADD/DEL_FILTER_*
                |  poll() -> EPOLLIN            |
================|===============================|===================================================
                |       /dev/syscall_monitor (Char Device, 176-byte Wire ABI)                       |
================|===============================|===================================================
                v                               |                                                   |
+-----------------------------------------------+---------------------------------------------------+
|                                     USER SPACE (C++23 TUI)                                        |
|                                                                                                   |
|   +-------------------------------------------------------------------------------------------+   |
|   |  main.cpp: one thread, one poll() loop                                                    |   |
|   |   ├── device fd, or the simulator's pipe if open fails (one generator thread)             |   |
|   |   └── STDIN_FILENO -> raw keystrokes, letter keys from a map of lambdas                   |   |
|   +------------------------------------+------------------------------------------------------+   |
|                                        |                                                          |
|                    +-------------------+-------------------+                                      |
|                    v                                       v                                      |
|       +-------------------------+             +-------------------------+                         |
|       |  main.cpp               |             |  filters.cpp            |                         |
|       |  - decode_events(span)  |             |  - filters.xml          |                         |
|       |  - 176 B alignment check|             |  - hide rules           |                         |
|       +------------+------------+             +------------+------------+                         |
|                    |                                       |                                      |
|                    +-------------------+-------------------+                                      |
|                                        v                                                          |
|       +---------------------------------------------------------------------------------------+   |
|       |  view.cpp: ring of 512 rows, 64-row pause queue, index stable_sort once per frame     |   |
|       |  capture.cpp: quote_pipe and the capture file                                         |   |
|       +--------------------------------+------------------------------------------------------+   |
|                                        |                                                          |
|                                        v                                                          |
|       +---------------------------------------------------------------------------------------+   |
|       |  renderer.cpp: ANSI bars, overlays, optional lsof screen                              |   |
|       +---------------------------------------------------------------------------------------+   |
+---------------------------------------------------------------------------------------------------+
```

### Kernel-to-userspace handshake

```text
User Space (C++23 TUI)                                  Kernel
──────────────────────                                  ------
open("/dev/syscall_monitor", O_RDONLY | O_NONBLOCK) ──> sm_open(): needs CAP_PERFMON or
                                                        CAP_SYS_ADMIN, else -EPERM;
                                                        sm_priv + snap, next_seq = seq
ioctl(fd, SET_EXCLUDE_PID, &self)                   ──> drop the TUI's own events
ioctl(fd, SET_FILTER_PID, &pid)   (optional, -p)    ──> priv->filter_pid (a process ID)
poll([{STDIN, POLLIN}, {fd, POLLIN}], 10ms)         ──> sm_poll(): POLLIN only if a record
                                                        this reader wants is pending
                                                    <── wake_up on sys_enter, if sleeping
read(fd, raw_buf, sizeof(raw_buf))                  ──> sm_read(): wanted records only,
                                                        whole records, never more than fit
decode_events(std::span<const uint8_t>)                 (userspace: reject size % 176 != 0)
```

If `open` fails (module not loaded, or not root and without `CAP_PERFMON`), the TUI prints the reason, `open_device` returns `std::unexpected(errno)`, and the TUI runs the simulator. Same screen, same keys, fake events. A failing `SET_FILTER_PID` is fatal rather than silently showing every process.

---

## 🔬 Kernel Driver

The module is one file, `syscall_monitor.c`. The device node stays `/dev/syscall_monitor`.

### 1. Atomic tracepoint hook

Before Linux 6.13 the `sys_enter` probe runs with preemption disabled, so sleeping, blocking locks, and `GFP_KERNEL` allocation are forbidden. From 6.13 the syscall tracepoints are faultable, but the probe is written for the stricter case on every kernel: it never sleeps and never takes a page fault.

Path arguments for `openat` (`args[1]`) and `execve` (`args[0]`) are user pointers. A normal copy can fault and sleep on I/O, which in atomic context is a `BUG: sleeping function called from invalid context`. The probe wraps `strncpy_from_user` in `pagefault_disable` / `pagefault_enable`. If the page is not resident, the copy fails and the string becomes `"[EFAULT]"`. (`strncpy_from_user_nofault` would be the tidier call, but it is not exported to modules.)

`comm` is copied with `strscpy` from `current->comm`; a task reading its own `comm` needs no lock. `pid` is the process ID (`task_tgid_nr`), the number `ps`, `lsof -p` and `-p` use, not the thread ID. Arguments come from `syscall_get_arguments`; kernels before 5.1 take extra index arguments, which is guarded.

For a 32-bit task on a 64-bit kernel (`in_compat_syscall()`), the syscall number is a compat number. The record sets `SM_NR_COMPAT` (`0x40000000`) in `nr`, keeps `args[0]`/`args[1]` raw, and skips path decoding. The TUI shows such rows as `compat_<nr>`.

### 2. Broadcast ring

4096 slots, 176 bytes each, indexed with `seq & SM_EVENT_RING_MASK`. The ring size is not part of the record ABI. The probe fills the record on its stack, then takes `raw_spin_lock_irqsave` (a real spinlock on `PREEMPT_RT` too) only to copy that one record into the ring and increment `seq`. It wakes the wait queue only when a reader is sleeping on it.

Each `open` gets an `sm_priv` with a mutex, its own cursor `next_seq`, its filters, and a ring-sized `snap` buffer. Readers take the ring lock once per record, so interrupts are never off for more than one 176-byte copy. If a reader falls behind by a full ring, its cursor skips to the oldest record still kept; the producer is never stalled.

`read`:

- copies only records this reader wants (see the filters below), whole records only, and never more than fit in the buffer;
- returns 0 for a zero-length read, and `-ENOBUFS` without consuming anything if the buffer is smaller than 176 bytes;
- when nothing wanted is pending, returns `-EAGAIN` with `O_NONBLOCK`, and otherwise blocks (interruptibly). It never returns 0, so a reader never sees a false end-of-file.

`poll` reports `POLLIN` only when a wanted record is pending, so a reader's own excluded syscalls do not wake it.

### 3. Per-open ioctl filters

- `SET_FILTER_PID` / `SET_EXCLUDE_PID`: one process ID each (0 = off).
- `SET_FILTER_SYSCALL` / `ADD_FILTER_SYSCALL` / `DEL_FILTER_SYSCALL`: up to `SM_MAX_NR_FILTERS` (64) ids, no duplicates on add. Ids are compared with the record's `nr`, so a compat syscall needs `SM_NR_COMPAT` set.
- `CLEAR_FILTERS`: clears pid, exclude, and the syscall list.
- Any other command returns `-ENOTTY`.

The argument is copied in before anything changes, and updates are serialised with the per-open mutex, so concurrent ioctls and reads on one descriptor are safe. 32-bit callers on a 64-bit kernel are served through `compat_ioctl`. Filtering happens in `sm_read`/`sm_poll` before the copy to userspace.

### 4. Device node and teardown

The stream shows every process's pids, command names, opened paths and executed files, so it is guarded like perf tracing. `/dev/syscall_monitor` keeps the kernel default mode, root-only `0600`, and `open` additionally requires `CAP_PERFMON` (5.8+) or `CAP_SYS_ADMIN`, else `-EPERM`. The device is not seekable (`lseek` gives `-ESPIPE`). `class_create` takes one argument on 6.4 and later, two on older kernels. Module init returns the real error code if a step fails, including `-ENOENT` when the `sys_enter` tracepoint is missing. `rmmod` is refused while the device is open. On unload the probe is unregistered and `tracepoint_synchronize_unregister` waits until no CPU is still inside it.

Online references:

- Tracepoints: https://docs.kernel.org/trace/tracepoints.html
- `syscall_get_arguments`: https://elixir.bootlin.com/linux/latest/ident/syscall_get_arguments
- Character devices: https://docs.kernel.org/driver-api/char-devices.html

---

## 🚀 C++23 in the TUI

Four features, each where a C++20 spelling was ambiguous. `std::print` is not used. GCC 12 does not ship `<print>`.

1. `std::span<const uint8_t>` in `decode_events`, in `main.cpp`. A pointer and a length can be swapped. A span cannot. A count that is not a multiple of 176 is rejected.
2. `std::expected<std::unique_ptr<Fd>, int>` from `open_device`. An empty `unique_ptr` used to mean "open failed". `expected` carries the errno. The caller still falls back to the simulator.
3. `std::to_underlying` converts `Key` to the integer the key parser returns.
4. `consteval sm_is_pow2` checks that the ring size (4096) is a power of two. `static_assert` checks that the record is 176 bytes.

Release builds use CMake's `-O3`, add `-fno-plt`, and turn on LTO when the toolchain supports it. `-march=native` is opt-in (`-DSYSWATCH_NATIVE=ON`), because such a binary may not run on another CPU.

Threading: the TUI runs on one thread. In simulator mode a generator thread writes 176-byte records into a pipe, and the main thread reads that pipe exactly like the device, so no data is shared between threads.

---

## 📁 Directory Layout

```text
.
├── LICENSE
├── THIRD_PARTY_NOTICES.md
├── README.md
├── .gitignore                 build/, filters.xml, capture logs
├── CMakeLists.txt             project SysWatch 1.2, C++23, binary syswatch, ctest
├── cmake/gen_syscall_names.cmake  syscall name table from <sys/syscall.h>
├── Makefile                   kernel module, objects in ./build/kmod
├── deploy.sh                  packages, build, insmod (removes the old 0666 udev rule)
├── syscall_monitor.c          kernel module
├── syscall_monitor_shared.h   176-byte ABI and ioctl numbers
├── main.cpp                   poll loop, keys, arguments, lsof
├── decode.hpp                 decode_events(span)
├── syscall_table.cpp / .hpp   syscall number -> name
├── text_util.cpp / .hpp       strip terminal control bytes from comm, paths, lsof
├── view.cpp / view.hpp        ring of rows, sort, pause
├── filters.cpp / filters.hpp  filters.xml
├── renderer.cpp / renderer.hpp terminal drawing
├── capture.cpp / capture.hpp  quote_pipe and the capture file
├── types.hpp                  UiEvent, Sort, Key, IView
├── mock_generator.hpp         fake events (through a pipe) when the device is missing
├── mock_test.cpp              tests: ABI, decoder, filters, sanitizer, syscall table, view
└── tests/qemu/                kernel module tests in a QEMU VM (see its README)

filters.xml is not tracked. The TUI writes a default one when it is missing,
and deploy.sh writes one from the running session if there is none.
```

There is no `monitor.cpp`. Decoding is in `decode.hpp`. Quoting is in `capture.cpp`.

---

## 🎮 Keyboard

Letter keys are a `std::map` of lambdas. Lookup is O(log N). Navigation keys go to the view; any other key is ignored.

| Key | Action |
|---|---|
| Up / Down | move the cursor; this pauses |
| PgUp / PgDn | page; this pauses |
| Home / End | jump to an edge; does not pause if you were not paused |
| Space | pause or resume; while paused up to 64 new rows are queued, later ones are dropped |
| F | filter hub: add, remove, show, from a list |
| S | sort column |
| O | ascending or descending |
| R | refresh interval, 100 to 10000 ms |
| L | lsof for the selected pid, if lsof is installed; lsof's own syscalls are hidden |
| C | start or stop the capture file |
| Esc | go back one panel in the filter hub, or close the open panel |
| Q | quit, restore the terminal |

Hide rules live in `./filters.xml` (the current directory), or in the file given with `-f`. The TUI creates it with defaults when it is missing and saves it whenever you add or remove a rule in the filter hub (up to 32 rules). Run with `sudo`, the file is written as root. A rule looks like this:

```xml
<hide kind="process">gnome-terminal*</hide>
<hide kind="pid">4812</hide>
<hide kind="syscall">read</hide>
<hide kind="sysid">0</hide>
```

- `process`: the command name, matched exactly on the 15 bytes the kernel keeps; a trailing `*` makes it a prefix match.
- `pid`: a process ID.
- `syscall`: the name shown in the Syscall column.
- `sysid`: the syscall number.

`&amp;`, `&lt;`, `&gt;`, `&quot;` and `&apos;` are understood and written back; comments are skipped; malformed rules are ignored.

The capture file is `strace_<host>_<yyyymmdd>_<hhmmss>.log` in the current directory, pipe-separated, with the time as nanoseconds since the epoch. Row times on screen are local time.

---

## 🧰 Toolchain

- Linux 5.4 or newer. The module builds without warnings (`W=1`) against 5.4, 5.10, 5.15, 6.1, 6.12 and 7.3 on x86_64, and was run on 6.12 and on 7.3 with `PREEMPT_RT`, KASAN and lockdep. `class_create` is guarded for 6.4, `perfmon_capable` for 5.8.
- Matching `linux-headers-$(uname -r)` if you want the module. Without headers the TUI still builds and runs in simulator mode.
- GCC 12 or newer (tested with GCC 14), or Clang with a C++23 `<expected>` (tested with Clang 19), for `<span>`, `<expected>`, `std::to_underlying`, `consteval`.
- CMake 3.20 or newer.
- Root, or `CAP_PERFMON` / `CAP_SYS_ADMIN`, to read the device. Without it the TUI runs in simulator mode.
- `lsof` is optional. Without it the `L` key is hidden.

Debian / Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake linux-headers-$(uname -r) lsof
```

Fedora / RHEL:

```bash
sudo dnf install -y gcc gcc-c++ cmake make kernel-devel-$(uname -r) lsof
```

---

## 🛠️ Build, Load, Run

`deploy.sh` is the normal path. It is idempotent and it does not start the TUI. Run it as a normal user (it uses `sudo` for packages and `insmod`) or as root.

```bash
./deploy.sh                    # build, load the module
./deploy.sh --no-run           # build only
./deploy.sh --refresh-filters  # also rewrite an existing filters.xml
```

By hand:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
make module
sudo insmod build/kmod/syscall_monitor.ko
sudo dmesg | tail -n 5
ls -l /dev/syscall_monitor
sudo ./build/syswatch
sudo ./build/syswatch -p 4812
sudo ./build/syswatch -f ~/my-filters.xml
```

`make module` copies the module sources into `./build/kmod` and runs kbuild with `M=./build/kmod`, so `.o` and `.cmd` files stay out of the source directory and away from the CMake build.

Expected kernel line:

```text
syscall_monitor: loaded (broadcast ring 4096, rec 176)
```

Flags are `-p <pid>` (show one process, pushed into the kernel), `-f <file>` (filter file) and `-h` (help). Anything else prints the usage and exits with status 2. There is no `--device`, `--syscall`, `--filter`, or `--output`.

Mock checks, no kernel required:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Kernel module tests in a VM, no root and no module on the host: [`tests/qemu`](tests/qemu/README.md) builds the module, static test programs and a static TUI, boots a (debug) kernel in QEMU with a busybox initramfs, and checks permissions, ioctls, read/poll semantics, 32-bit callers, the TUI on a pty, `rmmod`, and that the kernel log has no KASAN, lockdep or `BUG` reports.

```bash
KERNEL_SRC=/path/to/linux-6.12.x tests/qemu/build-kernel.sh   # once
KERNEL_SRC=/path/to/linux-6.12.x tests/qemu/run.sh
```

Unload:

```bash
sudo rmmod syscall_monitor
```

---

## 🔧 Troubleshooting

| Symptom | Cause | What to do |
|---|---|---|
| `Invalid module format` | headers do not match `uname -r` | install the matching headers and rebuild |
| `No such file` on the device | module not loaded | `sudo insmod build/kmod/syscall_monitor.ko` |
| `Permission denied` / `Operation not permitted` | not root and no `CAP_PERFMON` | `sudo ./build/syswatch`; do not make the node world-readable |
| insmod: `Unknown symbol` or `No such file or directory` | wrong kernel, or no `sys_enter` tracepoint | check `sudo dmesg` |
| `File exists` on insmod | module already loaded | `deploy.sh` unloads first, or `sudo rmmod` |
| Path is `[EFAULT]` | page not resident; the probe does not fault pages in | expected |
| `read` returns `-ENOBUFS` | buffer smaller than 176 bytes | size it as a multiple of 176 |
| `compat_<nr>` rows | a 32-bit process | expected; such syscalls are not decoded |
| TUI starts in simulator mode | device missing or not permitted | load the module, or keep the simulator |

---

**License**

This project is licensed under the [<u>MIT License</u>](https://github.com/mayday5000/SysWatch/blob/main/LICENSE). Copyright (c) 2026 Damian Andres Ulanowicz.

The kernel module declares `MODULE_LICENSE("Dual MIT/GPL")` so it can use GPL-only tracepoint symbols. Third-party components it builds against are listed in [THIRD_PARTY_NOTICES.md](https://github.com/mayday5000/SysWatch/blob/main/THIRD_PARTY_NOTICES.md).
