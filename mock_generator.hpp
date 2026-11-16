#pragma once
#include "syscall_monitor_shared.h"
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

/*
 * Fake event source for when /dev/syscall_monitor is missing.
 *
 * The worker thread never touches UI state. It writes whole 176-byte
 * sm_event records into a non-blocking pipe (each write is below PIPE_BUF,
 * so it is atomic), and the main loop polls and reads the pipe's read end
 * exactly like the device. All filtering, capture and view updates stay on
 * the main thread.
 */
class MockEventGenerator {
public:
    MockEventGenerator() = default;
    ~MockEventGenerator() { stop(); }

    MockEventGenerator(const MockEventGenerator &) = delete;
    MockEventGenerator &operator=(const MockEventGenerator &) = delete;

    // Starts the worker and returns the pipe's read end, or -1 on failure.
    int start() {
        if (running_) return rd_;
        int p[2];
        if (::pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
        rd_ = p[0];
        wr_ = p[1];
        running_ = true;
        worker_ = std::thread(&MockEventGenerator::loop, this);
        return rd_;
    }

    void stop() {
        if (running_.exchange(false) && worker_.joinable())
            worker_.join();
        if (wr_ >= 0) { ::close(wr_); wr_ = -1; }
        if (rd_ >= 0) { ::close(rd_); rd_ = -1; }
    }

    int fd() const { return rd_; }

private:
    void loop() {
        std::default_random_engine gen(static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<int32_t> pid_dist(1000, 9000);
        std::uniform_int_distribution<int32_t> nr_dist(1, 300);
        
        // Setup a seed library pool of processes to mimic scheduling contexts
        const std::vector<std::string> procs = {
            "nginx", "postgres", "redis-server", "chrome", "firefox", 
            "systemd", "dockerd", "node", "python3", "gnome-terminal"
        };
        std::uniform_int_distribution<size_t> proc_dist(0, procs.size() - 1);

        const std::vector<std::string> args = {
            "fd=3, buf=0x7ffe, count=1024",
            "path=\"/var/log/syslog\", flags=O_RDONLY",
            "epfd=5, events=0x55ab, maxevents=32",
            "pid=4102, sig=SIGTERM",
            "addr=0x7f83, length=4096, prot=PROT_READ"
        };
        std::uniform_int_distribution<size_t> arg_dist(0, args.size() - 1);

        uint64_t fake_timestamp = 1000000000;

        while (running_) {
            // Adjust loop sleep interval to regulate background event traffic throughput (e.g., ~15ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(15));

            sm_event ev{};
            ev.pid = pid_dist(gen);
            ev.nr = nr_dist(gen);
            ev.ts_ns = fake_timestamp += 15000000; // Increment mock nanoseconds cleanly
            ev.arg_a = 0xDEADBEEF;
            ev.arg_b = 0xCAFEBABE;

            std::string random_proc = procs[proc_dist(gen)];
            std::string random_arg  = args[arg_dist(gen)];

            std::strncpy(ev.comm, random_proc.c_str(), SM_COMM_LEN - 1);
            std::strncpy(ev.arg_str, random_arg.c_str(), SM_ARG_STR_LEN - 1);

            // A full pipe means the UI is behind: drop the record, like a
            // lapped reader of the kernel ring.
            [[maybe_unused]] ssize_t w = ::write(wr_, &ev, sizeof(ev));
        }
    }

    std::atomic<bool> running_{false};
    std::thread worker_;
    int rd_ = -1;
    int wr_ = -1;
};
