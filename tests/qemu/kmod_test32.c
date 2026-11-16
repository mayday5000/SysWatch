// SPDX-License-Identifier: MIT
/* 32-bit (compat) client of a 64-bit kernel: ioctls and compat records. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "syscall_monitor_shared.h"
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
int main(void) {
    static struct sm_event buf[SM_EVENT_RING_SIZE];
    printf("32-bit client: sizeof(sm_event)=%zu sizeof(long)=%zu\n", sizeof(struct sm_event), sizeof(long));
    int fd = open("/dev/syscall_monitor", O_RDONLY | O_NONBLOCK);
    CHECK(fd >= 0, "open");
    sm_s32 self = getpid();
    int r = ioctl(fd, SET_EXCLUDE_PID, &self);
    CHECK(r == 0, "compat SET_EXCLUDE_PID -> %d (%s)", r, r ? strerror(errno) : "ok");
    int pp[2]; pipe(pp);
    pid_t c = fork();
    if (c == 0) { char x; read(pp[0], &x, 1); for (int i = 0; i < 5; i++) syscall(SYS_getppid); _exit(0); }
    sm_s32 cp = c;
    CHECK(ioctl(fd, SET_FILTER_PID, &cp) == 0, "compat SET_FILTER_PID");
    while (read(fd, buf, sizeof(buf)) > 0) {}
    write(pp[1], "g", 1); int st; waitpid(c, &st, 0);
    int n = 0, flagged = 0, getppid_seen = 0; ssize_t k;
    while ((k = read(fd, buf, sizeof(buf))) > 0)
        for (ssize_t i = 0; i < k / (ssize_t)SM_EVENT_SIZE; i++) {
            n++;
            if (buf[i].nr & SM_NR_COMPAT) flagged++;
            if ((buf[i].nr & ~SM_NR_COMPAT) == SYS_getppid && (buf[i].nr & SM_NR_COMPAT) && buf[i].arg_str[0] == 0) getppid_seen++;
        }
    CHECK(n > 0 && flagged == n, "%d records from the 32-bit child, %d carry SM_NR_COMPAT", n, flagged);
    CHECK(getppid_seen == 5, "compat getppid (i386 nr %d) seen %d/5 times, arg_str undecoded", SYS_getppid, getppid_seen);
    printf("KMOD_TEST32 %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
