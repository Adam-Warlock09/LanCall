// Unit tests for audio::SpscRing (the lock-free queue between audio callbacks and our threads).
// Lives in tests/metrics/ because tests/audio/ is not part of the shared test layout.

#include <array>
#include <cstdint>
#include <thread>

#include "audio/spsc_ring.hpp"
#include "check.hpp"

using audio::SpscRing;

namespace {

void test_empty_pop_fails() {
    SpscRing<int, 8> r;
    int v = -1;
    CHECK(r.empty());
    CHECK(!r.pop(v));
    CHECK(v == -1);
}

void test_fifo_order() {
    SpscRing<int, 8> r;
    for (int i = 0; i < 5; ++i) CHECK(r.push(i));
    CHECK(r.size() == 5);
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        CHECK(r.pop(v));
        CHECK(v == i);
    }
    CHECK(r.empty());
}

void test_full_rejects_push() {
    SpscRing<int, 8> r;
    for (int i = 0; i < 8; ++i) CHECK(r.push(i));
    CHECK(r.size() == 8);
    CHECK(!r.push(99));  // full: rejected, nothing overwritten
    int v = -1;
    CHECK(r.pop(v));
    CHECK(v == 0);
    CHECK(r.push(8));  // room again
    for (int i = 1; i <= 8; ++i) {
        CHECK(r.pop(v));
        CHECK(v == i);
    }
    CHECK(r.empty());
}

void test_wraps_many_times() {
    SpscRing<int, 4> r;
    int next_in = 0, next_out = 0;
    for (int round = 0; round < 1000; ++round) {
        for (int k = 0; k < 3; ++k) CHECK(r.push(next_in++));
        for (int k = 0; k < 3; ++k) {
            int v = -1;
            CHECK(r.pop(v));
            CHECK(v == next_out++);
        }
    }
    CHECK(r.empty());
}

// Payload big enough to catch torn copies: every word must equal the sequence number.
struct Block {
    uint32_t seq = 0;
    std::array<uint32_t, 160> fill{};  // 644 bytes, like an audio chunk
};

void test_two_threads_stress() {
    static SpscRing<Block, 64> r;  // static: keep 41 KB off the stack
    constexpr uint32_t kCount = 300000;
    bool order_ok = true, content_ok = true;

    std::thread producer([] {
        Block b;
        for (uint32_t i = 0; i < kCount; ++i) {
            b.seq = i;
            b.fill.fill(i);
            while (!r.push(b)) std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        Block b;
        for (uint32_t expect = 0; expect < kCount;) {
            if (!r.pop(b)) {
                std::this_thread::yield();
                continue;
            }
            if (b.seq != expect) order_ok = false;
            for (uint32_t w : b.fill) {
                if (w != b.seq) content_ok = false;
            }
            ++expect;
        }
    });
    producer.join();
    consumer.join();
    CHECK(order_ok);
    CHECK(content_ok);
    CHECK(r.empty());
}

}  // namespace

int main() {
    RUN(test_empty_pop_fails);
    RUN(test_fifo_order);
    RUN(test_full_rejects_push);
    RUN(test_wraps_many_times);
    RUN(test_two_threads_stress);
    return check::result();
}
