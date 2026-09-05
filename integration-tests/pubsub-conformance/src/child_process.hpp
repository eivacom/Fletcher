// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A child process driven over one request/reply line protocol on its stdin and
// stdout. The whole reason the cross-process subjects exist: Fast DDS serves
// same-process endpoints over intra-process delivery, so a single-process suite
// cannot see the transport at all (spec §7.2).
//
// Every wait is bounded by a caller-supplied deadline. There is no retry, no
// reconnect and no partial mode: a child that crashes, hangs or closes its
// stdout makes the pending request return nullopt, which the subject turns into
// a typed failure and the clause fails on.

#ifndef FLETCHER_CONFORMANCE_CHILD_PROCESS_HPP_
#define FLETCHER_CONFORMANCE_CHILD_PROCESS_HPP_

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fletcher {
namespace conformance {

class ChildProcess {
   public:
    /// Spawns `exe` with `args`. Throws std::runtime_error if the process
    /// cannot be started — never a degraded object.
    ChildProcess(const std::string& exe, const std::vector<std::string>& args);
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Writes "<tag> <line>" and returns this request's reply with the tag
    /// stripped. Serialised: two threads may call it, and they queue rather
    /// than interleave on the pipe. nullopt on deadline or EOF.
    ///
    /// The tag is what keeps the stream from desyncing permanently. Without it,
    /// Request returns whatever line the child produced next — so one line of
    /// library logging on the child's stdout, or one late reply to a request
    /// whose deadline had already expired, shifts every later reply by one and a
    /// `create` that actually failed reports the previous request's `ok`. That
    /// is a silent false pass. With it, an untagged or wrongly-tagged line is
    /// not this request's reply and is discarded; a genuine desync then shows up
    /// as the deadline, which fails the clause loudly.
    std::optional<std::string> Request(const std::string& tag, const std::string& line,
                                       std::chrono::steady_clock::time_point deadline);

    /// Reads the child's next line without writing (the initial READY).
    std::optional<std::string> ReadLine(std::chrono::steady_clock::time_point deadline);

   private:
    void PumpStdout();
    void WriteLine(const std::string& line);
    void Shutdown();

    struct Platform;
    std::unique_ptr<Platform> plat_;

    std::mutex request_mu_;  // one request at a time

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    std::string partial_;
    bool eof_ = false;

    std::thread pump_;
};

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_CHILD_PROCESS_HPP_
