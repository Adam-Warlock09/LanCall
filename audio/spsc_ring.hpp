#pragma once
// SpscRing — fixed-size, lock-free queue for exactly ONE producer thread and ONE consumer
// thread (Person 2, audio/). Used between PortAudio callbacks and our own threads: audio
// callbacks must never block on a mutex or allocate memory, and this queue does neither.
//
//   producer: ring.push(item)  -> false if full (item not stored)
//   consumer: ring.pop(item)   -> false if empty
//
// N must be a power of two. Items are copied in and out, so keep T small and trivially
// copyable (an audio chunk is 644 bytes).

#include <array>
#include <atomic>
#include <cstddef>

namespace audio {

template <typename T, size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "SpscRing capacity must be a power of two");

public:
    // Producer only.
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail == N) return false;  // full
        buf_[head & (N - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (head == tail) return false;  // empty
        out = buf_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate when called while the other thread is active; exact otherwise.
    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    bool empty() const { return size() == 0; }
    static constexpr size_t capacity() { return N; }

private:
    // Separate cache lines so the producer and consumer don't slow each other down.
    alignas(64) std::atomic<size_t> head_{0};  // next slot to write (producer)
    alignas(64) std::atomic<size_t> tail_{0};  // next slot to read (consumer)
    alignas(64) std::array<T, N> buf_{};
};

}  // namespace audio
