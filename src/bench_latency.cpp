// Measures producer->consumer latency for two queues: the lock-free
// SpscRing and a mutex+condition_variable baseline. The point isn't
// throughput, it's the *distribution* — specifically the tail. A queue that
// looks fine on average and blows up at p99.9 is exactly the failure mode
// the "why C++ wins in finance" argument is about: predictability under
// load matters more than mean speed when missing a window by nanoseconds
// means someone else gets the fill.
//
// Each message carries its own send timestamp, so latency is measured
// end-to-end (time the producer decided to send -> time the consumer
// observed it), not just queue residency.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mutex_ring.hpp"
#include "spsc_ring.hpp"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Message {
    std::uint64_t seq;
    std::int64_t send_ns;
};

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

// Pins the *calling* thread, not a std::thread handle passed in from the
// outside. native_handle_type varies across standard library
// implementations (e.g. MinGW's pthreads-based std::thread hands back a
// pthread_t, not a Win32 HANDLE), so the portable approach is to have each
// worker pin itself as the first thing it does.
void pin_current_thread_to_core(int core) {
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << core);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#else
    (void)core;
#endif
}

// Busy-spin pacing: sleep_for has millisecond-scale granularity on most
// platforms, which would dominate the very latencies we're trying to
// measure. Spinning on the clock is the only way to hit microsecond-scale
// send intervals accurately from user space.
void spin_until(std::int64_t target_ns) {
    while (now_ns() < target_ns) {
        // busy wait
    }
}

double percentile(std::vector<std::int64_t>& sorted_ns, double p) {
    if (sorted_ns.empty()) return 0.0;
    std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(sorted_ns.size() - 1));
    return static_cast<double>(sorted_ns[idx]);
}

void print_stats(const char* label, std::vector<std::int64_t> latencies_ns) {
    std::sort(latencies_ns.begin(), latencies_ns.end());
    std::printf("\n%s (n=%zu)\n", label, latencies_ns.size());
    std::printf("  p50:    %8.0f ns\n", percentile(latencies_ns, 0.50));
    std::printf("  p90:    %8.0f ns\n", percentile(latencies_ns, 0.90));
    std::printf("  p99:    %8.0f ns\n", percentile(latencies_ns, 0.99));
    std::printf("  p99.9:  %8.0f ns\n", percentile(latencies_ns, 0.999));
    std::printf("  p99.99: %8.0f ns\n", percentile(latencies_ns, 0.9999));
    std::printf("  max:    %8lld ns\n", static_cast<long long>(latencies_ns.back()));
}

void write_csv(const std::string& path, const std::vector<std::int64_t>& latencies_ns) {
    std::ofstream out(path);
    out << "latency_ns\n";
    for (auto v : latencies_ns) out << v << "\n";
}

// ---- lock-free benchmark -------------------------------------------------

std::vector<std::int64_t> run_lockfree(std::uint64_t count, std::int64_t interval_ns) {
    static constexpr std::size_t kCapacity = 1 << 16;
    auto ring = std::make_unique<SpscRing<Message, kCapacity>>();
    std::vector<std::int64_t> latencies;
    latencies.reserve(count);

    std::thread producer([&] {
        pin_current_thread_to_core(0);
        std::int64_t next_send = now_ns();
        for (std::uint64_t seq = 0; seq < count; ++seq) {
            spin_until(next_send);
            Message msg{seq, now_ns()};
            while (!ring->try_push(msg)) {
                // ring full: spin. With a 64k-slot ring at these send
                // rates this should not happen in practice.
            }
            next_send += interval_ns;
        }
    });

    std::thread consumer([&] {
        pin_current_thread_to_core(1);
        Message msg{};
        for (std::uint64_t received = 0; received < count; ++received) {
            while (!ring->try_pop(msg)) {
                // spin
            }
            latencies.push_back(now_ns() - msg.send_ns);
        }
    });

    producer.join();
    consumer.join();
    return latencies;
}

// ---- mutex baseline -------------------------------------------------------

std::vector<std::int64_t> run_mutex(std::uint64_t count, std::int64_t interval_ns) {
    auto ring = std::make_unique<MutexRing<Message>>();
    std::vector<std::int64_t> latencies;
    latencies.reserve(count);

    std::thread producer([&] {
        pin_current_thread_to_core(0);
        std::int64_t next_send = now_ns();
        for (std::uint64_t seq = 0; seq < count; ++seq) {
            spin_until(next_send);
            ring->push(Message{seq, now_ns()});
            next_send += interval_ns;
        }
    });

    std::thread consumer([&] {
        pin_current_thread_to_core(1);
        for (std::uint64_t received = 0; received < count; ++received) {
            Message msg = ring->pop();
            latencies.push_back(now_ns() - msg.send_ns);
        }
    });

    producer.join();
    consumer.join();
    return latencies;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t lockfree_count = 1'000'000;
    std::uint64_t mutex_count = 200'000;
    std::int64_t interval_ns = 1'000; // ~1 message per microsecond

    if (argc > 1) lockfree_count = std::stoull(argv[1]);
    if (argc > 2) mutex_count = std::stoull(argv[2]);
    if (argc > 3) interval_ns = std::stoll(argv[3]);

    std::printf("send interval: %lld ns\n", static_cast<long long>(interval_ns));

#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    auto lockfree_latencies = run_lockfree(lockfree_count, interval_ns);
    print_stats("SpscRing (lock-free)", lockfree_latencies);
    write_csv("benchmarks/results/lockfree.csv", lockfree_latencies);

    auto mutex_latencies = run_mutex(mutex_count, interval_ns);
    print_stats("MutexRing (mutex + condition_variable)", mutex_latencies);
    write_csv("benchmarks/results/mutex.csv", mutex_latencies);

    return 0;
}
