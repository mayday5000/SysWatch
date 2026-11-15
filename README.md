# SysWatch

A high performance Linux kernel syscall telemetry engine and real-time C++23 TUI dashboard driven by atomic `sys_enter` tracepoint hooks.

Engineered around a **zero-allocation hot path** across the kernel, the system intercepts live `sys_enter` tracepoint events inside a non-sleeping atomic context, extracts syscall arguments safely without page faults, streams fixed-width **176-byte binary records** over `/dev/syscall_monitor` into a **512-slot broadcast ring**, and renders flicker-free real-time diagnostics at high event rates without inducing desktop or terminal sluggishness.

---

## ⚡ Zero-Allocation Hot Path Architecture

### What does "Zero-Allocation" mean?

Zero-allocation means that once the program finishes starting up, no dynamic heap memory is ever requested or freed (`kmalloc`/`kfree` in the kernel) while processing live syscall events.

Dynamic memory allocation is dangerous on a hot path for three reasons:

- **Unpredictable Latency (Jitter):** A heap allocator (`kmalloc`) sometimes has to search free lists, acquire global locks, or ask the OS for new memory pages, causing sudden micro-stutters.
- **Illegal in Kernel Atomic Context:** Inside `probe_sys_enter()`, sleeping is forbidden. A standard kernel allocation (`kmalloc(..., GFP_KERNEL)`) can sleep if RAM is low, which would trigger a fatal kernel panic.
- **Memory Fragmentation:** Allocating and freeing millions of tiny event structs per minute fragments memory and degrades CPU cache locality.

### How the Project Achieves It on the Kernel Side

| Layer | Cold Path (Allocates Once at Startup) | Hot Path (Zero Allocations per Event/Frame) |
| :--- | :--- | :--- |
| **Linux Kernel (`syscall_monitor.ko`)** | `sm_open()` calls `kzalloc()` and `kmalloc_array()` **once** when the TUI opens `/dev/syscall_monitor` to set up `priv->snap`. | `probe_sys_enter()` writes directly into a pre-allocated static circular array `mon_ctx.ring[512]` and a fixed stack buffer `char path[SM_ARG_STR_LEN]`. **Zero `kmalloc` calls per syscall.** |

---

## 🎯 Purpose & Why It Exists

Traditional user-space tracers such as `strace` rely on `ptrace`. That stops the target twice per syscall, forces a context switch on every entry and exit, and can only follow one process tree. Audit frameworks avoid the stop, but they add a daemon, a netlink socket, and a text record format.

SysWatch puts a short tracepoint probe in the kernel and streams compact binary records to an interactive C++23 terminal:

- **System-wide visibility.** Every process is visible. Nothing is stopped.
- **Several readers.** Each `open` of `/dev/syscall_monitor` has its own cursor and its own `ioctl` filters. Two TUIs, or a TUI and a logger, do not share a read position.
- **Live triage.** Pause, sort, scroll, hide rows with `filters.xml`, push a PID filter into the kernel, and write a capture file.

Scope is `sys_enter` only. There is no `sys_exit`, and argument decoding covers the path of `openat` and `execve` plus a tag for `mmap`. The kernel module is GPL because it links against the kernel. The userspace sources are MIT.

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
|                                                    | spin_lock_irqsave(&mon_ctx.lock)             |
|                                                    v                                              |
|   +-------------------------------------------------------------------------------------------+   |
|   |  mon_ctx.ring[512]   (Power-of-2 Broadcast Ring Buffer, 176 B / slot)                     |   |
|   |  [ Slot 0 ] [ Slot 1 ] [ Slot 2 ] ... [ head ] ... [ Slot 511 ]   (mon_ctx.seq++)         |   |
|   +----+-------------------------------+------------------------------------------------------+   |
|        | wake_up_interruptible(&waitq) |                                                          |
|        v                               v                                                          |
|   +-----------------------+       +-----------------------+                                       |
|   |  FD #1 (sm_priv)      |       |  FD #2 (sm_priv)      |   Per-Reader State:                   |
|   |  - read_index         |       |  - read_index         |   - Independent ring cursor           |
|   |  - last_seq           |       |  - last_seq           |   - Dedicated snap[] staging buffer   |
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
|   |  main.cpp: Single-Thread Event Multiplexer via poll()                                     |   |
|   |   ├── device fd, or the simulator if open fails                                           |   |
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
|       |  view.cpp: ring of 512 rows, pause queue, index stable_sort                           |   |
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
open("/dev/syscall_monitor", O_RDONLY | O_NONBLOCK) ──> sm_open(): sm_priv + snap
                                                        last_seq = current seq
ioctl(fd, SET_EXCLUDE_PID, &self)                   ──> drop the TUI's own events
ioctl(fd, SET_FILTER_PID, &pid)   (optional, -p)    ──> WRITE_ONCE(priv->filter_pid)
poll([{fd, POLLIN}, {STDIN, POLLIN}], 10ms)         ──> sm_poll(): seq != last_seq
                                                    <── wake_up on sys_enter -> POLLIN
read(fd, raw_buf, sizeof(raw_buf))                  ──> sm_read(): whole records only
decode_events(std::span<const uint8_t>)             ──> reject if size % 176 != 0
```

If `open` fails, `open_device` returns `std::unexpected(errno)` and the TUI runs the simulator. Same screen, same keys, fake events.

---

## 🔬 Kernel Driver

The module is one file, `syscall_monitor.c`. The device node stays `/dev/syscall_monitor`.

### 1. Atomic tracepoint hook

When the kernel calls the `sys_enter` probe, the CPU is in atomic context. Sleeping, blocking locks, and `GFP_KERNEL` allocation are forbidden.

Path arguments for `openat` (`args[1]`) and `execve` (`args[0]`) are user pointers. A normal copy can fault and try to sleep on I/O, which is `BUG: scheduling while atomic`. The probe wraps `strncpy_from_user` in `pagefault_disable` / `pagefault_enable`. If the page is not resident, the copy returns `-EFAULT` and the string becomes `"[EFAULT]"`.

`comm` is copied with `strscpy` from `current->comm`. `get_task_comm` is not used, because it takes a sleeping lock. Arguments come from `syscall_get_arguments`. Kernels before 5.1 take five arguments; 5.1 and later take three.

### 2. Broadcast ring

512 slots, 176 bytes each, indexed with `& SM_EVENT_RING_MASK`. Under `spin_lock_irqsave` the probe writes one record, advances `head`, increments `seq`, drops the lock, and wakes the wait queue.

Each `open` gets an `sm_priv` with its own `read_index`, `last_seq`, and `snap`. If a reader falls behind by a full ring, `sm_read` snaps forward and drops the stale records. The producer is not stalled.

### 3. Per-open ioctl filters

- `SET_FILTER_PID` / `SET_EXCLUDE_PID`: `WRITE_ONCE` / `READ_ONCE`.
- `SET_FILTER_SYSCALL` / `ADD_FILTER_SYSCALL` / `DEL_FILTER_SYSCALL`: up to `SM_MAX_NR_FILTERS` ids, no duplicates on add.
- `CLEAR_FILTERS`: clears pid, exclude, and the syscall list.

Filtering happens in `sm_read` before the copy to userspace.

### 4. Device node and teardown

`/dev/syscall_monitor` is mode `0666` from `sm_devnode`. `class_create` takes one argument on 6.4 and later, two on older kernels. On `rmmod`, the probe is unregistered and `tracepoint_synchronize_unregister` waits until no CPU is still inside it.

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
4. `consteval sm_is_pow2` checks that 512 is a power of two. `static_assert` checks that the record is 176 bytes.

Release builds pass `-O3` and `-march=native`, and turn on LTO when the toolchain supports it.

---

## 📁 Directory Layout

```text
.
├── LICENSE
├── README.md
├── CMakeLists.txt             project SysWatch, C++23, binary syswatch
├── Makefile                   kernel module, objects in ./build
├── deploy.sh                  packages, build, udev, insmod
├── syscall_monitor.c          kernel module
├── syscall_monitor_shared.h   176-byte ABI and ioctl numbers
├── main.cpp                   poll loop, decode_events, keys
├── view.cpp / view.hpp        ring of rows, sort, pause
├── filters.cpp / filters.hpp  filters.xml
├── renderer.cpp / renderer.hpp terminal drawing
├── capture.cpp / capture.hpp  quote_pipe and the capture file
├── types.hpp                  UiEvent, Sort, Key, IView
├── mock_generator.hpp         fake events when the device is missing
├── mock_test.cpp              ABI, span decode, XML parse checks
├── syscall_names.inc          syscall names, rewritten by deploy.sh
└── filters.xml                hide rules, rewritten by deploy.sh
```

There is no `monitor.cpp`. Decoding is in `main.cpp`. Quoting is in `capture.cpp`.

---

## 🎮 Keyboard

Letter keys are a `std::map` of lambdas. Lookup is O(log N). A key that is not in the map goes to the view.

| Key | Action |
|---|---|
| Up / Down | move the cursor; this pauses |
| PgUp / PgDn | page |
| Home / End | jump to an edge; does not pause if you were not paused |
| Space | pause or resume |
| F | filter hub: add, remove, show, from a list |
| S | sort column |
| O | ascending or descending |
| R | refresh interval, 100 to 10000 ms |
| L | lsof for the selected pid, if lsof is installed |
| C | start or stop the capture file |
| Q | quit, restore the terminal |

The filter file is not typed as `<PID>1234</PID>`. A rule looks like this:

```xml
<hide kind="process">gnome-terminal</hide>
<hide kind="pid">4812</hide>
<hide kind="syscall">read</hide>
<hide kind="sysid">0</hide>
```

The capture file is `strace_<host>_<yyyymmdd>_<hhmmss>.log`, pipe-separated.

---

## 🧰 Toolchain

- Linux, kernel 5.4 or newer. `syscall_get_arguments` is guarded for kernels before 5.1. `class_create` is guarded for 6.4.
- Matching `linux-headers-$(uname -r)` if you want the module. Without headers the TUI still builds and runs in simulator mode.
- GCC 12 or Clang 15, for `<span>`, `<expected>`, `std::to_underlying`, `consteval`.
- CMake 3.16 or newer.
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

`deploy.sh` is the normal path. It is idempotent and it does not start the TUI.

```bash
./deploy.sh
./deploy.sh --no-run
```

By hand:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
make module
sudo insmod build/syscall_monitor.ko
dmesg | tail -n 5
ls -l /dev/syscall_monitor
./build/syswatch
./build/syswatch -p 4812
```

`make module` copies the module sources into `./build` and runs kbuild with `M=./build`, so `.o` and `.cmd` files stay out of the source directory.

Expected kernel line:

```text
syscall_monitor: loaded (broadcast ring 512, rec 176)
```

Flags are `-p <pid>` and `-h`. There is no `--device`, `--syscall`, `--filter`, or `--output`.

Mock checks, no kernel required:

```bash
g++ -std=c++23 -Wall -Wextra -O2 -I. -o mock_test mock_test.cpp filters.cpp
./mock_test
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
| `No such file` on the device | module not loaded | `sudo insmod build/syscall_monitor.ko` |
| `Permission denied` | udev overrode 0666 | `sudo chmod 0666 /dev/syscall_monitor` |
| `File exists` on insmod | module already loaded | `deploy.sh` unloads first, or `sudo rmmod` |
| Path is `[EFAULT]` | page not resident in the atomic probe | expected |
| `read` returns `-ENOBUFS` | buffer smaller than 176 bytes | size it as a multiple of 176 |
| TUI starts in simulator mode | device missing or not permitted | load the module, or keep the simulator |

---

**License**

This project is licensed under the [<u>MIT License</u>](https://github.com/mayday5000/SysWatch/blob/main/LICENSE). Copyright (c) 2026 Damian Andres Ulanowicz.
