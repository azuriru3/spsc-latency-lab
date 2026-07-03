#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

// Single-producer/single-consumer ring buffer. No locks, no syscalls, no
// allocation after construction. Correct only under exactly one producer
// thread and one consumer thread — that constraint is what makes the
// lock-free algorithm this simple.
//
// head_/tail_ each get their own cache line, and each thread keeps a private
// cached copy of the *other* side's index so the hot path only touches the
// shared atomic when it actually looks like the ring is full/empty. Without
// that, every push/pop would bounce the other thread's cache line — the
// exact kind of hidden latency spike this repo exists to measure.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    bool try_push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        if (head - cached_tail_ == Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head - cached_tail_ == Capacity) {
                return false; // full
            }
        }

        buffer_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) {
                return false; // empty
            }
        }

        out = buffer_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Producer-owned line: head_ is written by the producer and read by the
    // consumer; cached_tail_ is private to the producer and lives here
    // specifically so it never shares a line with tail_ (see below).
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::size_t cached_tail_{0};

    // Consumer-owned line: mirror image of the above.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLineSize) std::size_t cached_head_{0};

    alignas(kCacheLineSize) T buffer_[Capacity];
};
