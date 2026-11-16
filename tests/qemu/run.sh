#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the module, the static test programs and the static TUI, pack them
# into a busybox initramfs, boot a kernel in QEMU, and check the results.
#
#   KERNEL_SRC=/path/to/linux tests/qemu/run.sh
#   BZIMAGE=/boot/vmlinuz-X KDIR=/lib/modules/X/build tests/qemu/run.sh
#
# Environment:
#   KERNEL_SRC  built kernel tree; default for KDIR and BZIMAGE
#   KDIR        tree to build the module against (default $KERNEL_SRC,
#               else /lib/modules/$(uname -r)/build)
#   BZIMAGE     kernel image to boot (default $KERNEL_SRC/arch/x86/boot/bzImage)
#   KO          use this prebuilt syscall_monitor.ko instead of building one
#   BUSYBOX     statically linked busybox (default: busybox on PATH)
#   OUT         work directory (default tests/qemu/out)
#   ACCEL       QEMU accelerator: tcg (default) or kvm
#   VMTIMEOUT   seconds before the VM is killed (default 600)
#   QEMU_MEM, QEMU_SMP, APPEND  memory (2048), CPUs (4), extra kernel args
#   SKIP_TUI=1  do not build or run the TUI test
#   SKIP_32=1   do not build or run the 32-bit test
# Exit status: 0 when every check passed and the kernel log is clean.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$HERE/out}"
CC="${CC:-gcc}"

die() { echo "run.sh: $*" >&2; exit 2; }
step() { echo "--- $*"; }

if [[ -n "${KERNEL_SRC:-}" ]]; then
    KDIR="${KDIR:-$KERNEL_SRC}"
    BZIMAGE="${BZIMAGE:-$KERNEL_SRC/arch/x86/boot/bzImage}"
fi
KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
[[ -n "${BZIMAGE:-}" ]] || die "set BZIMAGE or KERNEL_SRC"
[[ -r "$BZIMAGE" ]] || die "kernel image not readable: $BZIMAGE"

BUSYBOX="${BUSYBOX:-$(command -v busybox || true)}"
[[ -x "$BUSYBOX" ]] || die "busybox not found; install busybox-static or set BUSYBOX"
if ldd "$BUSYBOX" >/dev/null 2>&1; then
    die "$BUSYBOX is dynamically linked; install busybox-static or set BUSYBOX"
fi
command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 not found"
command -v cpio >/dev/null || die "cpio not found"

rm -rf "$OUT"
mkdir -p "$OUT"/{kmod,bin,root/bin,root/proc,root/sys,root/dev,root/tmp}

# 1. kernel module
if [[ -n "${KO:-}" ]]; then
    cp "$KO" "$OUT/kmod/syscall_monitor.ko"
else
    step "building the module against $KDIR"
    cp "$REPO/syscall_monitor.c" "$REPO/syscall_monitor_shared.h" "$OUT/kmod/"
    # shellcheck disable=SC2016  # $(src) is expanded by kbuild
    printf 'obj-m += syscall_monitor.o\nccflags-y := -I$(src)\n' > "$OUT/kmod/Kbuild"
    symwarn=()
    [[ -s "$KDIR/Module.symvers" ]] || symwarn=(KBUILD_MODPOST_WARN=1)
    if ! make -C "$KDIR" M="$OUT/kmod" W=1 "${symwarn[@]}" modules > "$OUT/kmod/build.log" 2>&1; then
        cat "$OUT/kmod/build.log"; die "module build failed"
    fi
    nwarn=$(grep -c 'warning:' "$OUT/kmod/build.log" || true)
    echo "module built, $nwarn compiler warnings (log: $OUT/kmod/build.log)"
fi

# 2. static test programs
step "building static test programs"
"$CC" -static -O2 -Wall -Wextra -I"$REPO" -o "$OUT/bin/kmod_test" "$HERE/kmod_test.c" -lpthread
"$CC" -static -O2 -Wall -o "$OUT/bin/noise" "$HERE/noise.c"
"$CC" -static -O2 -Wall -o "$OUT/bin/ptyrun" "$HERE/ptyrun.c" -lutil
if [[ "${SKIP_32:-0}" != 1 ]]; then
    if ! "$CC" -m32 -static -O2 -Wall -I"$REPO" -o "$OUT/bin/kmod_test32" "$HERE/kmod_test32.c" 2>"$OUT/bin/m32.log"; then
        echo "note: no 32-bit toolchain (gcc-multilib); skipping the 32-bit test"
        rm -f "$OUT/bin/kmod_test32"
    fi
fi

# 3. static TUI
if [[ "${SKIP_TUI:-0}" != 1 ]]; then
    step "building a static syswatch"
    cmake -S "$REPO" -B "$OUT/tui" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
          -DCMAKE_EXE_LINKER_FLAGS=-static > "$OUT/tui.log" 2>&1 || { cat "$OUT/tui.log"; die "cmake configure failed"; }
    cmake --build "$OUT/tui" -j"$(nproc 2>/dev/null || echo 2)" >> "$OUT/tui.log" 2>&1 || { cat "$OUT/tui.log"; die "TUI build failed"; }
fi

# 4. initramfs
step "packing the initramfs"
R="$OUT/root"
cp "$BUSYBOX" "$R/bin/busybox"
cp "$HERE/init" "$R/init"; chmod +x "$R/init"
cp "$OUT/kmod/syscall_monitor.ko" "$R/"
cp "$OUT/bin/kmod_test" "$OUT/bin/noise" "$OUT/bin/ptyrun" "$R/"
[[ -x "$OUT/bin/kmod_test32" ]] && cp "$OUT/bin/kmod_test32" "$R/"
[[ -x "$OUT/tui/syswatch" ]] && cp "$OUT/tui/syswatch" "$R/syswatch"
(cd "$R" && find . | cpio -o -H newc --quiet | gzip) > "$OUT/initrd.gz"

# 5. boot
ACCEL="${ACCEL:-tcg}"
step "booting $BZIMAGE (accel=$ACCEL, timeout ${VMTIMEOUT:-600}s)"
timeout "${VMTIMEOUT:-600}" qemu-system-x86_64 -accel "$ACCEL" -m "${QEMU_MEM:-2048}" \
    -smp "${QEMU_SMP:-4}" -cpu max -kernel "$BZIMAGE" -initrd "$OUT/initrd.gz" \
    -append "console=ttyS0 panic=-1 loglevel=4 ${APPEND:-}" -display none \
    -serial "file:$OUT/serial.log" -no-reboot > "$OUT/qemu.log" 2>&1 || true

# 6. verdict
LOG="$OUT/serial.log"
[[ -s "$LOG" ]] || die "no serial output (see $OUT/qemu.log)"
tr -d '\r' < "$LOG" > "$OUT/serial.txt"
sed -n '/=== insmod/,/=== DONE/p' "$OUT/serial.txt"
echo
fail=0
check() { if grep -qE "$2" "$OUT/serial.txt"; then echo "PASS  $1"; else echo "FAIL  $1"; fail=1; fi; }
check "module loaded"                  '^insmod-ok'
check "device node is root-only 0600"  '^crw------- +1 0 +0 .*/dev/syscall_monitor'
check "kmod_test"                      'KMOD_TEST PASSED'
[[ -x "$OUT/bin/kmod_test32" ]] && check "kmod_test32 (compat)" 'KMOD_TEST32 PASSED'
[[ -x "$OUT/tui/syswatch" ]] && check "TUI on a pty" 'PTYRUN PASSED'
check "rmmod refused while open"       '^rmmod-busy-ok'
check "rmmod"                          '^rmmod-ok'
check "VM reached the end"             '^=== DONE'
bad=$(grep -E 'BUG:|WARNING:|KASAN:|UBSAN:|possible circular locking|possible recursive locking|inconsistent lock state|sleeping function called|scheduling while atomic|Kernel panic|Oops' "$OUT/serial.txt" || true)
if [[ -n "$bad" ]]; then echo "FAIL  kernel log clean:"; echo "$bad"; fail=1; else echo "PASS  kernel log clean (no BUG/WARNING/KASAN/UBSAN/lockdep reports)"; fi
echo
if [[ "$fail" -eq 0 ]]; then echo "QEMU TESTS PASSED"; else echo "QEMU TESTS FAILED (full log: $LOG)"; fi
exit "$fail"
