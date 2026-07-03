// Deliberately not assert()-based: CMakeLists.txt builds Release by
// default, which defines NDEBUG and compiles every assert() to nothing.
// A test suite that silently checks nothing is worse than no test suite.
// CHECK() below always evaluates and reports, regardless of build type.
//
// Lock-free code is exactly the kind of thing that "looks right" and is
// subtly wrong under concurrency, so these tests cover both: single-
// threaded API contract (full/empty/wraparound) and a real two-thread run
// checked for dropped, duplicated, or reordered messages.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "spsc_ring.hpp"

namespace {
int g_failures = 0;
}

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

void test_empty_ring_reports_empty() {
    SpscRing<int, 4> ring;
    int out = 0;
    CHECK(!ring.try_pop(out));
    std::puts("test_empty_ring_reports_empty: done");
}

void test_push_pop_preserves_order() {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        CHECK(ring.try_push(i));
    }
    for (int i = 0; i < 4; ++i) {
        int out = -1;
        CHECK(ring.try_pop(out));
        CHECK(out == i);
    }
    std::puts("test_push_pop_preserves_order: done");
}

void test_full_ring_rejects_push() {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        CHECK(ring.try_push(i));
    }
    int overflow = 99;
    CHECK(!ring.try_push(overflow));
    std::puts("test_full_ring_rejects_push: done");
}

void test_wraparound_across_many_cycles() {
    // Capacity 4, push/pop 100 items one at a time so head/tail wrap
    // around the buffer many times over. Order must survive the wrap.
    SpscRing<int, 4> ring;
    for (int i = 0; i < 100; ++i) {
        CHECK(ring.try_push(i));
        int out = -1;
        CHECK(ring.try_pop(out));
        CHECK(out == i);
    }
    std::puts("test_wraparound_across_many_cycles: done");
}

void test_concurrent_producer_consumer_no_loss_no_reorder() {
    constexpr std::uint64_t kCount = 1'000'000;
    SpscRing<std::uint64_t, 1 << 12> ring;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring.try_push(i)) {
                // spin
            }
        }
    });

    std::uint64_t expected = 0;
    std::thread consumer([&] {
        std::uint64_t value = 0;
        while (expected < kCount) {
            if (ring.try_pop(value)) {
                CHECK(value == expected);
                ++expected;
            }
        }
    });

    producer.join();
    consumer.join();
    CHECK(expected == kCount);
    std::puts("test_concurrent_producer_consumer_no_loss_no_reorder: done");
}

int main() {
    test_empty_ring_reports_empty();
    test_push_pop_preserves_order();
    test_full_ring_rejects_push();
    test_wraparound_across_many_cycles();
    test_concurrent_producer_consumer_no_loss_no_reorder();

    if (g_failures == 0) {
        std::puts("\nall tests passed");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
