#include "runity/core/task_queue.hpp"

namespace runity::core {

void TaskQueue::post(Task task) {
    std::lock_guard lock(mutex_);
    tasks_.push_back(std::move(task));
}

std::size_t TaskQueue::run_pending() {
    std::deque<Task> batch;
    {
        std::lock_guard lock(mutex_);
        batch.swap(tasks_);
    }
    for (auto& task : batch) task();
    return batch.size();
}

std::size_t TaskQueue::size() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

Worker::Worker() : thread_([this] { loop(); }) {}

Worker::~Worker() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void Worker::submit(Task job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void Worker::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

void Worker::loop() {
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) {
            if (stopping_) return;
            continue;
        }
        Task job = std::move(jobs_.front());
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        job();
        lock.lock();
        busy_ = false;
        if (jobs_.empty()) idle_.notify_all();
    }
}

}  // namespace runity::core
