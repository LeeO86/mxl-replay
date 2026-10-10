// The built-in MediaMTX's supervisor. From mxl-webrtc-monitor 1.3.0 src/ops/child.hpp
// (MIT, Copyright (c) 2026 Adrian Hilber); see NOTICE.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace replay
{
// A child process (the built-in MediaMTX) that is started again when it exits: after 1 s, doubling
// to 10 s, and after 1 s again once it ran for 10 s. It gets SIGTERM when this process dies, and its
// own process group, so a terminal's Ctrl-C reaches only this process. stop() sends SIGTERM and,
// after the grace time, SIGKILL.
class ChildProcess
{
public:
    explicit ChildProcess(std::string name);
    ~ChildProcess();

    ChildProcess(ChildProcess const&) = delete;
    ChildProcess& operator=(ChildProcess const&) = delete;

    // argv[0] is looked up in PATH.
    void start(std::vector<std::string> argv);
    void stop(int graceMs = 3000);
    bool running() const;
    // Starts after the first one.
    std::uint64_t restarts() const;

private:
    void run();
    pid_t spawn();

    std::string name_;
    std::vector<std::string> argv_;
    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    pid_t pid_ = 0; // a started child that is not reaped yet, so its pid is not reused
    std::atomic<std::uint64_t> restarts_{0};
};
} // namespace replay
