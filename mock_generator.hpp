#pragma once
#include "syscall_monitor_shared.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <random>
#include <cstring>

class MockEventGenerator {
public:
    MockEventGenerator() : running_(false) {}
    
    ~MockEventGenerator() {
        stop();
    }

    // Spawns a dedicated parallel worker thread to pump dummy traffic frames
    void start(int(*callback)(const sm_event&, void*), void* user_data) {
        if (running_) return;
        running_ = true;
        worker_ = std::thread(&MockEventGenerator::loop, this, callback, user_data);
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void loop(int(*callback)(const sm_event&, void*), void* user_data) {
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

            // Execute processing loop dispatch via the standard architecture hook
            if (callback) {
                callback(ev, user_data);
            }
        }
    }

    std::atomic<bool> running_;
    std::thread worker_;
};
