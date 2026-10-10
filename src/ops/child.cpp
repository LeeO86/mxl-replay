// The built-in MediaMTX's supervisor. From mxl-webrtc-monitor 1.3.0 src/ops/child.cpp
// (MIT, Copyright (c) 2026 Adrian Hilber); see NOTICE.
#include "ops/child.hpp"

#include "util/logging.hpp"

#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace replay
{
ChildProcess::ChildProcess(std::string name)
    : name_(std::move(name))
{
}

ChildProcess::~ChildProcess()
{
    stop();
}

void ChildProcess::start(std::vector<std::string> argv)
{
    argv_ = std::move(argv);
    thread_ = std::thread([this] { run(); });
}

void ChildProcess::stop(int graceMs)
{
    {
        std::lock_guard const lock{mu_};
        stop_ = true;
        if (pid_ > 0)
        {
            ::kill(pid_, SIGTERM);
        }
    }
    cv_.notify_all();
    if (!thread_.joinable())
    {
        return;
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
    for (;;)
    {
        {
            std::lock_guard const lock{mu_};
            if (pid_ == 0)
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                ::kill(pid_, SIGKILL);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    thread_.join();
}

bool ChildProcess::running() const
{
    std::lock_guard const lock{mu_};
    return pid_ > 0;
}

std::uint64_t ChildProcess::restarts() const
{
    return restarts_.load();
}

pid_t ChildProcess::spawn()
{
    std::vector<char*> args;
    for (auto& arg : argv_)
    {
        args.push_back(arg.data());
    }
    args.push_back(nullptr);
    std::string const failed = name_ + ": cannot execute " + argv_.front() + "\n";
    pid_t const parent = ::getpid();
    pid_t const pid = ::fork();
    if (pid != 0)
    {
        return pid;
    }
    // The child: only async-signal-safe calls until exec.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != parent)
    {
        ::_exit(1);
    }
    ::setpgid(0, 0);
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
    {
        for (int fd = 3; fd < 1024; ++fd)
        {
            ::close(fd);
        }
    }
    ::execvp(args[0], args.data());
    auto const written = ::write(2, failed.data(), failed.size());
    (void)written;
    ::_exit(127);
}

void ChildProcess::run()
{
    int delayMs = 1000;
    bool first = true;
    for (;;)
    {
        pid_t pid = 0;
        int spawnError = 0;
        {
            std::lock_guard const lock{mu_};
            if (stop_)
            {
                return;
            }
            pid = spawn();
            spawnError = pid < 0 ? errno : 0;
            if (pid > 0)
            {
                pid_ = pid;
            }
        }
        if (pid < 0)
        {
            logError(name_ + "_spawn_failed", {{"error", std::strerror(spawnError)}});
        }
        else
        {
            if (!first)
            {
                restarts_.fetch_add(1);
            }
            first = false;
            logInfo(name_ + "_started", {{"pid", std::to_string(pid)}});
            auto const started = std::chrono::steady_clock::now();
            // Wait without reaping: until pid_ is cleared, stop() can still signal this pid.
            siginfo_t info{};
            while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) != 0 && errno == EINTR)
            {
            }
            bool stopping = false;
            {
                std::lock_guard const lock{mu_};
                pid_ = 0;
                ::waitpid(pid, nullptr, 0);
                stopping = stop_;
            }
            auto const status = (info.si_code == CLD_EXITED ? "exit " : "signal ") + std::to_string(info.si_status);
            if (stopping)
            {
                logInfo(name_ + "_stopped", {{"status", status}});
                return;
            }
            if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(10))
            {
                delayMs = 1000;
            }
            logWarn(name_ + "_exited", {{"status", status}, {"restart_in_ms", std::to_string(delayMs)}});
        }
        std::unique_lock lock{mu_};
        if (cv_.wait_for(lock, std::chrono::milliseconds(delayMs), [this] { return stop_; }))
        {
            return;
        }
        delayMs = std::min(delayMs * 2, 10000);
    }
}
} // namespace replay
