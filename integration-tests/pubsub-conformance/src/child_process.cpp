// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "child_process.hpp"

#include <csignal>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
// clang-format off
#include <windows.h>
// clang-format on
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace fletcher {
namespace conformance {

#ifdef _WIN32

struct ChildProcess::Platform {
    HANDLE to_child = nullptr;    // our write end of the child's stdin
    HANDLE from_child = nullptr;  // our read end of the child's stdout
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
};

namespace {

/// Quotes one argument the way CommandLineToArgvW parses it back.
std::string QuoteArg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

}  // namespace

ChildProcess::ChildProcess(const std::string& exe, const std::vector<std::string>& args)
    : plat_(std::make_unique<Platform>()) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    if (!CreatePipe(&child_stdin_read, &plat_->to_child, &sa, 0)) {
        throw std::runtime_error("conformance: CreatePipe failed for peer " + exe);
    }
    if (!CreatePipe(&plat_->from_child, &child_stdout_write, &sa, 0)) {
        // Close the first pair before unwinding; plat_ is not yet in a state
        // Shutdown() can clean up from, since the pump thread does not exist.
        CloseHandle(child_stdin_read);
        CloseHandle(plat_->to_child);
        plat_->to_child = nullptr;
        throw std::runtime_error("conformance: CreatePipe failed for peer " + exe);
    }
    // Our own ends must not be inherited, or the child holds them open and the
    // pump never sees EOF.
    SetHandleInformation(plat_->to_child, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(plat_->from_child, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = QuoteArg(exe);
    for (const std::string& a : args) {
        cmd += ' ';
        cmd += QuoteArg(a);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    if (!ok) {
        const DWORD err = GetLastError();
        CloseHandle(plat_->to_child);
        CloseHandle(plat_->from_child);
        plat_->to_child = nullptr;
        plat_->from_child = nullptr;
        throw std::runtime_error("conformance: cannot start peer " + exe +
                                 " (CreateProcess error " + std::to_string(err) + ")");
    }
    plat_->process = pi.hProcess;
    plat_->thread = pi.hThread;

    pump_ = std::thread([this] { PumpStdout(); });
}

void ChildProcess::PumpStdout() {
    char buf[512];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(plat_->from_child, buf, sizeof(buf), &read, nullptr) || read == 0) {
            break;
        }
        {
            std::lock_guard lock(mu_);
            for (DWORD i = 0; i < read; ++i) {
                if (buf[i] == '\n') {
                    if (!partial_.empty() && partial_.back() == '\r') {
                        partial_.pop_back();
                    }
                    lines_.push_back(std::move(partial_));
                    partial_.clear();
                } else {
                    partial_ += buf[i];
                }
            }
        }
        cv_.notify_all();
    }
    {
        std::lock_guard lock(mu_);
        eof_ = true;
    }
    cv_.notify_all();
}

void ChildProcess::WriteLine(const std::string& line) {
    const std::string payload = line + "\n";
    DWORD written = 0;
    WriteFile(plat_->to_child, payload.data(), static_cast<DWORD>(payload.size()), &written,
              nullptr);
}

void ChildProcess::Shutdown() {
    if (plat_->to_child != nullptr) {
        CloseHandle(plat_->to_child);
        plat_->to_child = nullptr;
    }
    if (plat_->process != nullptr) {
        // The peer exits on stdin EOF. Bounded: a hung child is terminated
        // rather than hanging the suite.
        if (WaitForSingleObject(plat_->process, 5000) != WAIT_OBJECT_0) {
            TerminateProcess(plat_->process, 1);
            WaitForSingleObject(plat_->process, 5000);
        }
    }
    if (pump_.joinable()) {
        pump_.join();
    }
    if (plat_->from_child != nullptr) {
        CloseHandle(plat_->from_child);
        plat_->from_child = nullptr;
    }
    if (plat_->thread != nullptr) {
        CloseHandle(plat_->thread);
        plat_->thread = nullptr;
    }
    if (plat_->process != nullptr) {
        CloseHandle(plat_->process);
        plat_->process = nullptr;
    }
}

#else  // POSIX

struct ChildProcess::Platform {
    int to_child = -1;
    int from_child = -1;
    pid_t pid = -1;
};

ChildProcess::ChildProcess(const std::string& exe, const std::vector<std::string>& args)
    : plat_(std::make_unique<Platform>()) {
    // Writing to a dead child must return nullopt, which is what this class
    // promises and what the refusal ladder requires. Without this, SIGPIPE's
    // default disposition TERMINATES the whole gtest binary on the next
    // WriteLine after the peer dies: no named clause failure, no remaining
    // clauses in that binary, just exit=141. Process-wide and idempotent, so
    // several ChildProcess instances setting it is harmless.
    std::signal(SIGPIPE, SIG_IGN);

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0) {
        throw std::runtime_error("conformance: pipe() failed for peer " + exe);
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        throw std::runtime_error("conformance: pipe() failed for peer " + exe);
    }

    std::vector<std::string> argv_storage;
    argv_storage.push_back(exe);
    for (const std::string& a : args) {
        argv_storage.push_back(a);
    }
    std::vector<char*> argv;
    for (std::string& s : argv_storage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        throw std::runtime_error("conformance: fork() failed for peer " + exe);
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        // Everything above stderr goes: the parent's provider was constructed
        // BEFORE the spawn (make() is a constructor argument), so without this
        // the peer inherits its DDS/XRCE sockets and shared-memory handles.
        // close() is async-signal-safe, which is all that may run between fork
        // and execv in a multi-threaded parent.
        const long max_fd = sysconf(_SC_OPEN_MAX);
        for (int fd = STDERR_FILENO + 1; fd < static_cast<int>(max_fd > 0 ? max_fd : 1024); ++fd) {
            close(fd);
        }
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    plat_->to_child = in_pipe[1];
    plat_->from_child = out_pipe[0];
    plat_->pid = pid;
    // Our ends must not survive into a LATER child: one holding another child's
    // stdin write end means closing ours never produces EOF there, and teardown
    // pays the full kill path instead of exiting cleanly.
    fcntl(plat_->to_child, F_SETFD, FD_CLOEXEC);
    fcntl(plat_->from_child, F_SETFD, FD_CLOEXEC);

    pump_ = std::thread([this] { PumpStdout(); });
}

void ChildProcess::PumpStdout() {
    char buf[512];
    for (;;) {
        const ssize_t n = read(plat_->from_child, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        {
            std::lock_guard lock(mu_);
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    if (!partial_.empty() && partial_.back() == '\r') {
                        partial_.pop_back();
                    }
                    lines_.push_back(std::move(partial_));
                    partial_.clear();
                } else {
                    partial_ += buf[i];
                }
            }
        }
        cv_.notify_all();
    }
    {
        std::lock_guard lock(mu_);
        eof_ = true;
    }
    cv_.notify_all();
}

void ChildProcess::WriteLine(const std::string& line) {
    const std::string payload = line + "\n";
    size_t off = 0;
    while (off < payload.size()) {
        const ssize_t n = write(plat_->to_child, payload.data() + off, payload.size() - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            // EPIPE (the child is gone) and every other write error are the same
            // thing here: the request cannot be delivered. Return, and let the
            // caller's bounded ReadLine turn it into the nullopt this class
            // promises. SIGPIPE is ignored in the constructor, so EPIPE arrives
            // as a return value rather than as process death.
            return;
        }
        off += static_cast<size_t>(n);
    }
}

void ChildProcess::Shutdown() {
    if (plat_->to_child >= 0) {
        close(plat_->to_child);
        plat_->to_child = -1;
    }
    if (plat_->pid > 0) {
        // Bounded reap: the peer exits on stdin EOF, and a hung one is killed.
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int status = 0;
        for (;;) {
            const pid_t r = waitpid(plat_->pid, &status, WNOHANG);
            if (r == plat_->pid || r < 0) {
                break;
            }
            if (std::chrono::steady_clock::now() >= give_up) {
                kill(plat_->pid, SIGKILL);
                waitpid(plat_->pid, &status, 0);
                break;
            }
            // Sleep, not yield: a tight waitpid(WNOHANG)+yield loop burned a
            // whole core for the full 5 s on a hanging child, ~26 times a run,
            // on the same loaded runner the suite needs to be quick on.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        plat_->pid = -1;
    }
    if (pump_.joinable()) {
        pump_.join();
    }
    if (plat_->from_child >= 0) {
        close(plat_->from_child);
        plat_->from_child = -1;
    }
}

#endif

ChildProcess::~ChildProcess() { Shutdown(); }

std::optional<std::string> ChildProcess::ReadLine(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mu_);
    if (!cv_.wait_until(lock, deadline, [this] { return !lines_.empty() || eof_; })) {
        return std::nullopt;
    }
    if (lines_.empty()) {
        return std::nullopt;  // EOF: the child died or closed stdout
    }
    std::string line = std::move(lines_.front());
    lines_.pop_front();
    return line;
}

std::optional<std::string> ChildProcess::Request(const std::string& tag, const std::string& line,
                                                 std::chrono::steady_clock::time_point deadline) {
    std::lock_guard serialise(request_mu_);
    WriteLine(tag + " " + line);

    const std::string prefix = tag + " ";
    for (;;) {
        std::optional<std::string> got = ReadLine(deadline);
        if (!got.has_value()) {
            return std::nullopt;  // deadline or EOF
        }
        if (got->rfind(prefix, 0) == 0) {
            return got->substr(prefix.size());
        }
        // Not this request's reply: the child's own stdout noise, or a reply to
        // a request that already timed out. Discarded, not mistaken for ours.
    }
}

}  // namespace conformance
}  // namespace fletcher
