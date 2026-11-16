#ifndef SYSCALL_MONITOR_SHARED_H
#define SYSCALL_MONITOR_SHARED_H

/*
 * syscall_monitor_shared.h
 * Purpose: the wire ABI shared by the kernel module and the TUI.
 * One packed little-endian 176-byte record. Same layout on x86, x86_64,
 * arm64, and riscv64, including a 32-bit process on a 64-bit kernel.
 * Do not replace sm_* with pid_t or long.
 *
 * There is no per-arch branch for the record. The same struct is
 * the wire format everywhere. Syscall numbers come from the headers
 * of the architecture being compiled, and a missing header is an error.
 *
 * Typical numbers, for reading only. The macros below do not hardcode them.
 *   x86    openat 295   mmap2 192   execve 11
 *   x86_64 openat 257   mmap    9   execve 59
 *   arm64  openat  56   mmap  222   execve 221
 *   riscv64 openat 56   mmap  222   execve 221
 */

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#error "syscall_monitor requires __BYTE_ORDER__ (x86 / x86_64 / arm64 / riscv64 LE)"
#endif
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "syscall_monitor ABI is little-endian (x86 / x86_64 / arm64 / riscv64 LE)"
#endif

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
typedef __s32 sm_s32;
typedef __u32 sm_u32;
typedef __u64 sm_u64;
#else
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <stddef.h>
typedef int32_t  sm_s32;
typedef uint32_t sm_u32;
typedef uint64_t sm_u64;
#endif

#define SM_IOC_MAGIC            'k'
#define SET_FILTER_PID          _IOW(SM_IOC_MAGIC, 1, sm_s32)
#define SET_FILTER_SYSCALL      _IOW(SM_IOC_MAGIC, 2, sm_s32)
#define CLEAR_FILTERS           _IO(SM_IOC_MAGIC, 3)
#define SET_EXCLUDE_PID         _IOW(SM_IOC_MAGIC, 4, sm_s32)
#define ADD_FILTER_SYSCALL      _IOW(SM_IOC_MAGIC, 5, sm_s32)
#define DEL_FILTER_SYSCALL      _IOW(SM_IOC_MAGIC, 6, sm_s32)
#define SM_MAX_NR_FILTERS       (64)

/*
 * Set in sm_event.nr for a syscall made by a 32-bit (compat) task on a
 * 64-bit kernel, for example an i386 binary on x86_64. The low bits are
 * then the compat syscall number (i386 openat is 295, not 257), and
 * arg_str is not decoded because the number does not mean the same call.
 * SET/ADD/DEL_FILTER_SYSCALL compare the full value, flag included.
 */
#define SM_NR_COMPAT            (0x40000000)

#define SM_COMM_LEN             (16)
#define SM_ARG_STR_LEN          (128)
#define SM_EVENT_RING_SIZE      (512)
#define SM_EVENT_RING_MASK      (SM_EVENT_RING_SIZE - 1)
#define SM_EVENT_SIZE           (176)    /* 4+4+8+8+8+16+128 */

#if defined(__GNUC__) || defined(__clang__)
#define SM_PACKED               __attribute__((packed))
#else
#error "syscall_monitor requires a packed sm_event"
#endif

/*
 * Kernel: <asm/unistd.h> of the ARCH the module is built for.
 * Userspace: <sys/syscall.h> of the architecture the TUI is built for.
 * mmap2 is the 32-bit x86 call (offset in pages). x86_64, arm64, and
 * riscv64 have no mmap2, so they use mmap.
 */
#ifdef __KERNEL__
#include <asm/unistd.h>

#if !defined(__NR_openat) || !defined(__NR_execve) || (!defined(__NR_mmap2) && !defined(__NR_mmap))
#error "kernel headers are missing unistd bindings for this architecture"
#endif

#define SM_NR_OPENAT            __NR_openat
#define SM_NR_EXECVE            __NR_execve

#if defined(__NR_mmap2)
#define SM_NR_MMAP              __NR_mmap2   /* x86 32-bit */
#else
#define SM_NR_MMAP              __NR_mmap    /* x86_64, arm64, riscv64 */
#endif

#else /* userspace */

#if !defined(SYS_openat) || !defined(SYS_execve) || (!defined(SYS_mmap2) && !defined(SYS_mmap))
#error "userspace headers are missing syscall numbers for this architecture"
#endif

#define SM_NR_OPENAT            SYS_openat
#define SM_NR_EXECVE            SYS_execve

#if defined(SYS_mmap2)
#define SM_NR_MMAP              SYS_mmap2    /* x86 32-bit */
#else
#define SM_NR_MMAP              SYS_mmap     /* x86_64, arm64, riscv64 */
#endif

#endif

#ifdef __cplusplus
extern "C" {
#endif

struct sm_event {
    sm_s32 pid;
    sm_s32 nr;
    sm_u64 ts_ns;
    sm_u64 arg_a;
    sm_u64 arg_b;
    char   comm[SM_COMM_LEN];
    char   arg_str[SM_ARG_STR_LEN];
} SM_PACKED;

#ifdef __cplusplus
}
#endif

#ifdef __KERNEL__
#include <linux/build_bug.h>
static_assert(sizeof(struct sm_event) == SM_EVENT_SIZE, "sm_event ABI size drifted");
static_assert(offsetof(struct sm_event, ts_ns) == 8, "sm_event ts_ns offset drifted");
static_assert(offsetof(struct sm_event, arg_str) == 48, "sm_event arg_str offset drifted");
#else
#include <assert.h>
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__cplusplus)
static_assert(sizeof(struct sm_event) == SM_EVENT_SIZE, "sm_event ABI size drifted");
static_assert(offsetof(struct sm_event, ts_ns) == 8, "sm_event ts_ns offset drifted");
static_assert(offsetof(struct sm_event, arg_str) == 48, "sm_event arg_str offset drifted");
#else
typedef char __sm_abi_size[sizeof(struct sm_event) == SM_EVENT_SIZE ? 1 : -1];
typedef char __sm_abi_ts[offsetof(struct sm_event, ts_ns) == 8 ? 1 : -1];
typedef char __sm_abi_str[offsetof(struct sm_event, arg_str) == 48 ? 1 : -1];
#endif
#endif

#endif /* SYSCALL_MONITOR_SHARED_H */
