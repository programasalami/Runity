#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace waw::core {

using Task = std::move_only_function<void()>;

/// Thread-safe queue of tasks that ONE owner thread runs when it chooses (the simulation thread drains it once per tick).
/// Background work (database, Redis) posts its completion here, so game state is only ever touched on the simulation thread.
class TaskQueue {
public:
    void post(Task task);
    /// Runs every task queued so far (tasks posted while draining run on the next drain). Returns how many ran.
    std::size_t run_pending();
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<Task> tasks_;
};

/// One background thread that runs submitted jobs in order. The destructor finishes every job already submitted, then joins:
/// nothing queued (e.g. a final character save) is silently dropped at shutdown.
class Worker {
public:
    Worker();
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void submit(Task job);
    /// Blocks until every job submitted before this call has finished.
    void wait_idle();

private:
    void loop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Task> jobs_;
    bool busy_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace waw::core
