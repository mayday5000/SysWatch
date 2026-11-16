#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Configure and build a debug kernel for the QEMU tests.
#   KERNEL_SRC=/path/to/linux-6.12.x tests/qemu/build-kernel.sh
# Optional: KCONFIG_FRAGMENTS (default "debug.config", relative to this
# directory or absolute), JOBS (default nproc).
# Produces $KERNEL_SRC/arch/x86/boot/bzImage and Module.symvers.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${KERNEL_SRC:?set KERNEL_SRC to an unpacked kernel source tree}"
FRAGS="${KCONFIG_FRAGMENTS:-debug.config}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

paths=()
for f in $FRAGS; do
    [[ "$f" = /* ]] || f="$HERE/$f"
    [[ -f "$f" ]] || { echo "missing config fragment: $f" >&2; exit 1; }
    paths+=("$f")
done

cd "$KERNEL_SRC"
make -s x86_64_defconfig
scripts/kconfig/merge_config.sh -m .config "${paths[@]}" >/dev/null
make -s olddefconfig
echo "Key options:"
grep -E '^CONFIG_(PREEMPT|PREEMPT_RT|KASAN|PROVE_LOCKING|DEBUG_ATOMIC_SLEEP|UBSAN|FTRACE_SYSCALLS|IA32_EMULATION)=' .config || true
make -s -j"$JOBS" bzImage modules
echo "Built $KERNEL_SRC/arch/x86/boot/bzImage"
