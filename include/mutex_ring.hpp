#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>

// The baseline everyone reaches for first: a mutex, a condition variable,
// and a deque. Correct, simple, and exactly what the benchmark exists to
// put a number on: every push takes a lock, every pop can block on the OS
// scheduler to wake the consumer thread back up, and the deque allocates.
// None of that is a bug, it's just what "not lock-free" costs.
template <typename T>
class MutexRing {
public:
    void push(const T& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(value);
        }
        cv_.notify_one();
    }

    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        T value = queue_.front();
        queue_.pop_front();
        return value;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
};
