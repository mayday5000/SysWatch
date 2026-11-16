// SPDX-License-Identifier: MIT
/* ptyrun: run syswatch on a pty inside the VM, press keys, check the screen */
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static char out[1 << 22]; static size_t olen;
static void pump(int fd, int ms) {
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        long el = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms) return;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, (int)(ms - el)) > 0) { ssize_t n = read(fd, out + olen, sizeof(out) - olen - 1); if (n <= 0) return; olen += (size_t)n; }
    }
}
int main(int argc, char **argv) {
    struct winsize ws = { 30, 120, 0, 0 }; int fd; int fails = 0;
    pid_t c = forkpty(&fd, NULL, NULL, &ws);
    if (c == 0) { setenv("TERM", "xterm", 1); chdir("/tmp"); execv(argv[1], argv + 1); _exit(127); }
    pump(fd, 4000);
    const char *keys[] = { " ", "\x1b[B", "\x1b[A", "\x1b[H", "\x1b[F", "s", "\r", "f", "\x1b", "\x1b", "c", "c", " " };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) { write(fd, keys[i], strlen(keys[i])); pump(fd, 500); }
    pump(fd, 1500);
    write(fd, "q", 1); pump(fd, 1500);
    int st = -1; pid_t w = 0;
    for (int i = 0; i < 50 && !(w = waitpid(c, &st, WNOHANG)); i++) pump(fd, 100);
    printf("child %s, status 0x%x\n", w == c ? "exited" : "still running", st);
    if (!w) { kill(c, SIGKILL); waitpid(c, &st, 0); }
    out[olen] = 0;
#define CK(cond, msg) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "ok  " : "FAIL", msg); fails += !ok_; } while (0)
    CK(strstr(out, "Syscall Monitor v1.2") != NULL, "TUI title v1.2 drawn");
    CK(strstr(out, "Simulator") == NULL, "TUI used the real device (no simulator fallback)");
    CK(strstr(out, "ptyrun") != NULL && strstr(out, "noise") != NULL, "real process names (ptyrun, noise) in rows");
    CK(strstr(out, "openat") != NULL, "syscall names resolved (openat)");
    CK(strstr(out, "/proc/version") != NULL, "openat path decoded (/proc/version)");
    CK(strstr(out, "syswatch ") == NULL, "TUI's own syscalls excluded");
    CK(w == c && WIFEXITED(st) && WEXITSTATUS(st) == 0, "q exits with status 0");
    CK(strstr(out, "\033[?1049l") != NULL, "terminal restored (alt screen left)");
    FILE *f = popen("ls /tmp/strace_*.log 2>/dev/null | head -n1", "r"); char name[256] = "";
    if (f) { if (!fgets(name, sizeof name, f)) name[0] = 0; pclose(f); }
    CK(name[0] != 0, "capture file written");
    if (name[0]) { name[strcspn(name, "\n")] = 0; char cmd[512]; snprintf(cmd, sizeof cmd, "wc -l < %s; head -n 3 %s", name, name); system(cmd); snprintf(cmd, sizeof cmd, "echo capture: $(grep -c openat %s) openat rows, $(grep -c /proc/version %s) with /proc/version, $(grep -c \"|syswatch|\" %s) from syswatch itself", name, name, name); system(cmd); }
    printf("PTYRUN %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
