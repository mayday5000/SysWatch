// SPDX-License-Identifier: MIT
/* noise: a background process with a known name that opens /proc/version
 * every 100 ms, so the TUI test has openat rows with a decoded path. */
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
    for (;;) {
        int f = open("/proc/version", O_RDONLY);
        if (f >= 0)
            close(f);
        usleep(100000);
    }
}
