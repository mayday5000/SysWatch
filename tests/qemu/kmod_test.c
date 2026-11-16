// SPDX-License-Identifier: MIT
/* kmod_test: in-VM checks of /dev/syscall_monitor (permissions, filters,
 * read/poll semantics, tgid, lapped readers, concurrent ioctls, ENOTTY,
 * ESPIPE). Run as root inside the test VM; prints ok/FAIL lines and
 * "KMOD_TEST PASSED|FAILED". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "syscall_monitor_shared.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static int devopen(int fl) { return open("/dev/syscall_monitor", O_RDONLY | fl); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int g_fd;
static void *adder(void *a) {
    (void)a;
    for (int i = 0; i < 2000; i++) { sm_s32 v = i % 200; ioctl(g_fd, ADD_FILTER_SYSCALL, &v); if (i % 7 == 0) ioctl(g_fd, DEL_FILTER_SYSCALL, &v); }
    return NULL;
}
static volatile int g_stop;
static long g_reads;
static void *reader(void *a) {
    (void)a; static __thread struct sm_event rb[64];
    while (!g_stop) { struct pollfd p = { g_fd, POLLIN, 0 }; poll(&p, 1, 1); if (read(g_fd, rb, sizeof(rb)) > 0) __sync_fetch_and_add(&g_reads, 1); }
    return NULL;
}
static void *noise(void *a) { (void)a; while (!g_stop) { int f = open("/proc/self/stat", O_RDONLY); if (f >= 0) close(f); } return NULL; }
static void on_alarm(int s) { (void)s; }
static void *thread_getppid(void *a) { (void)a; syscall(SYS_getppid); return NULL; }

int main(void) {
    static struct sm_event buf[SM_EVENT_RING_SIZE];
    printf("sizeof(sm_event)=%zu ring=%d\n", sizeof(struct sm_event), SM_EVENT_RING_SIZE);

    /* 1. permissions */
    pid_t c;
    struct stat sb; stat("/dev/syscall_monitor", &sb);
    CHECK((sb.st_mode & 0777) == 0600 && sb.st_uid == 0, "node is root 0600 (mode %o)", sb.st_mode & 0777);
    int st;
    for (int pass = 0; pass < 2; pass++) {
        /* pass 1 simulates the old 0666 udev rule: the capability check must still refuse */
        if (pass == 1) chmod("/dev/syscall_monitor", 0666);
        c = fork();
        if (c == 0) { if (setgid(65534) || setuid(65534)) _exit(3); int fd = devopen(0); _exit(fd >= 0 ? 1 : errno == EPERM ? 10 : errno == EACCES ? 11 : 2); }
        waitpid(c, &st, 0);
        CHECK(WEXITSTATUS(st) == (pass ? 10 : 11), "unprivileged open %s -> %s", pass ? "of a 0666 node" : "of the 0600 node", WEXITSTATUS(st) == 10 ? "EPERM" : WEXITSTATUS(st) == 11 ? "EACCES" : "opened/other");
    }
    chmod("/dev/syscall_monitor", 0600);
    int fd = devopen(O_NONBLOCK);
    CHECK(fd >= 0, "root open (%s)", fd < 0 ? strerror(errno) : "fd ok");
    if (fd < 0) return 1;

    /* 2. self-wakeup: idle system, own syscalls excluded */
    sm_s32 self = getpid();
    CHECK(ioctl(fd, SET_EXCLUDE_PID, &self) == 0, "SET_EXCLUDE_PID");
    while (read(fd, buf, sizeof(buf)) > 0) {}
    int wakes = 0, loops = 0; double t0 = now();
    while (now() - t0 < 2.0) {
        struct pollfd p = { fd, POLLIN, 0 };
        loops++;
        if (poll(&p, 1, 200) > 0) { wakes++; while (read(fd, buf, sizeof(buf)) > 0) {} }
    }
    CHECK(loops < 100, "idle poll loop: %d iterations, %d readable wakes in 2s (no self-wakeup spin)", loops, wakes);

    /* 3. openat capture + pid filter + small reads lose nothing */
    int pp[2]; pipe(pp);
    c = fork();
    if (c == 0) { static const char path[] = "/swtest/path-123"; char x; read(pp[0], &x, 1); (void)*(volatile const char *)path; for (int i = 0; i < 10; i++) { int f = openat(AT_FDCWD, path, O_RDONLY); if (f >= 0) close(f); } _exit(0); }
    sm_s32 cp = c;
    CHECK(ioctl(fd, SET_FILTER_PID, &cp) == 0, "SET_FILTER_PID child");
    sm_s32 nr = SM_NR_OPENAT;
    CHECK(ioctl(fd, SET_FILTER_SYSCALL, &nr) == 0, "SET_FILTER_SYSCALL openat");
    while (read(fd, buf, sizeof(buf)) > 0) {}
    write(pp[1], "g", 1); waitpid(c, &st, 0);
    char small[100];
    CHECK(read(fd, small, sizeof(small)) < 0 && errno == ENOBUFS, "read(100 bytes) -> ENOBUFS");
    int got = 0, okpath = 0; ssize_t r;
    while ((r = read(fd, buf, SM_EVENT_SIZE)) > 0) {
        if (r != SM_EVENT_SIZE) { printf("short %zd\n", r); break; }
        got++; if (buf[0].pid == c && buf[0].nr == SM_NR_OPENAT && strcmp(buf[0].arg_str, "/swtest/path-123") == 0) okpath++; else printf("     record pid=%d nr=%d str='%s'\n", buf[0].pid, buf[0].nr, buf[0].arg_str);
    }
    CHECK(!(buf[0].nr & SM_NR_COMPAT), "native records carry no SM_NR_COMPAT");
    CHECK(got == 10 && okpath == 10, "one-record reads: %d events, %d with pid/nr/path right (want 10/10)", got, okpath);
    CHECK(r < 0 && errno == EAGAIN, "drained -> EAGAIN");

    /* 3b. records carry the process ID (tgid), also for a second thread */
    {
        int p2[2]; pipe(p2);
        pid_t c2 = fork();
        if (c2 == 0) { char x; read(p2[0], &x, 1); pthread_t t; pthread_create(&t, NULL, thread_getppid, NULL); pthread_join(t, NULL); _exit(0); }
        sm_s32 cp2 = c2; ioctl(fd, CLEAR_FILTERS); ioctl(fd, SET_FILTER_PID, &cp2);
        sm_s32 nr2 = SYS_getppid; ioctl(fd, SET_FILTER_SYSCALL, &nr2);
        while (read(fd, buf, sizeof(buf)) > 0) {}
        write(p2[1], "g", 1); waitpid(c2, &st, 0);
        r = read(fd, buf, sizeof(buf));
        CHECK(r == SM_EVENT_SIZE && buf[0].pid == c2, "thread's getppid reported with the process ID %d (got %zd bytes, pid %d)", (int)c2, r, r > 0 ? buf[0].pid : -1);
        ioctl(fd, CLEAR_FILTERS); ioctl(fd, SET_EXCLUDE_PID, &self);
    }

    /* 4. blocking read with nothing wanted: EINTR, never 0 */
    int bfd = devopen(0);
    sm_s32 nobody = 999999; ioctl(bfd, SET_FILTER_PID, &nobody);
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, NULL);
    alarm(1); r = read(bfd, buf, sizeof(buf)); int e = errno; alarm(0);
    CHECK(r < 0 && e == EINTR, "blocking read, all filtered -> %zd (%s), not 0/EOF", r, strerror(e));

    /* 5. lapping: burst bigger than the ring */
    ioctl(fd, CLEAR_FILTERS); ioctl(fd, SET_EXCLUDE_PID, &self);
    while (read(fd, buf, sizeof(buf)) > 0) {}
    c = fork();
    if (c == 0) { for (int i = 0; i < SM_EVENT_RING_SIZE * 3; i++) syscall(SYS_getppid); _exit(0); }
    waitpid(c, &st, 0);
    long total = 0; while ((r = read(fd, buf, sizeof(buf))) > 0) total += r / SM_EVENT_SIZE;
    CHECK(total > 0 && total <= SM_EVENT_RING_SIZE, "lapped reader got %ld records (<= ring %d)", total, SM_EVENT_RING_SIZE);

    /* 6. concurrent ioctl stress on one fd */
    {
        int sfd = devopen(O_NONBLOCK);   /* fresh fd, no exclude: sees the noise threads */
        g_fd = sfd; pthread_t th[8], rd[2], nz[2];
        for (int i = 0; i < 2; i++) { pthread_create(&rd[i], NULL, reader, NULL); pthread_create(&nz[i], NULL, noise, NULL); }
        for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, adder, NULL);
        for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
        g_stop = 1;
        for (int i = 0; i < 2; i++) { pthread_join(rd[i], NULL); pthread_join(nz[i], NULL); }
        CHECK(ioctl(sfd, CLEAR_FILTERS) == 0, "8 ADD/DEL threads + 2 readers + 2 syscall-noise threads on one fd survived (%ld non-empty reads)", g_reads);
        close(sfd);
    }
    /* 6b. two readers each get the same record */
    {
        int f1 = devopen(O_NONBLOCK), f2 = devopen(O_NONBLOCK);
        pid_t c3 = fork();
        if (c3 == 0) { char x; read(0, &x, 0); usleep(200000); syscall(SYS_getppid); _exit(0); }
        sm_s32 cp3 = c3, nr3 = SYS_getppid;
        ioctl(f1, SET_FILTER_PID, &cp3); ioctl(f2, SET_FILTER_PID, &cp3);
        ioctl(f1, SET_FILTER_SYSCALL, &nr3); ioctl(f2, SET_FILTER_SYSCALL, &nr3);
        waitpid(c3, &st, 0);
        ssize_t r1 = read(f1, buf, sizeof(buf)), r2 = read(f2, buf, sizeof(buf));
        CHECK(r1 == SM_EVENT_SIZE && r2 == SM_EVENT_SIZE, "two readers each got the record (%zd, %zd bytes)", r1, r2);
        close(f1); close(f2);
    }
    CHECK(ioctl(fd, _IO(SM_IOC_MAGIC, 77)) < 0 && errno == ENOTTY, "unknown ioctl -> ENOTTY (%s)", strerror(errno));
    CHECK(lseek(fd, 0, SEEK_SET) < 0 && errno == ESPIPE, "lseek -> ESPIPE");
    close(bfd);
    printf("KMOD_TEST %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
