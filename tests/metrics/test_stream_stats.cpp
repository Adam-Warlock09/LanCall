// Unit tests for metrics::StreamStats (loss, wrap, reorder, duplicates, jitter, rates, restart).

#include <cstdint>
#include <string>
#include <thread>

#include "check.hpp"
#include "metrics/stream_stats.hpp"

using metrics::StreamStats;

namespace {

constexpr uint32_t kStep = 20000;  // 20 ms between packets, like audio
constexpr size_t kBytes = 652;     // 12-byte header + 640-byte audio payload

// Feed one packet sent at index i: timestamp i*20 ms, arriving a constant 5 ms later.
void feed(StreamStats& s, uint16_t seq, uint32_t i, uint32_t extra_delay_us = 0) {
    const uint32_t ts = i * kStep;
    s.on_packet(seq, ts, kBytes, ts + 5000 + extra_delay_us);
}

void test_no_loss() {
    StreamStats s("audio");
    for (uint32_t i = 0; i < 1000; ++i) feed(s, static_cast<uint16_t>(i), i);
    const auto t = s.total();
    CHECK(t.expected == 1000);
    CHECK(t.received == 1000);
    CHECK(t.lost == 0);
    CHECK(t.loss_pct == 0.0);
    CHECK(t.reordered == 0);
    CHECK(t.duplicates == 0);
    CHECK_NEAR(t.jitter_ms, 0.0, 1e-12);  // constant network delay -> no jitter
}

void test_known_loss() {
    StreamStats s("audio");
    for (uint32_t i = 0; i < 1000; ++i) {
        if (i % 100 == 7) continue;  // drop 7, 107, ..., 907 -> 10 packets
        feed(s, static_cast<uint16_t>(i), i);
    }
    const auto t = s.total();
    CHECK(t.expected == 1000);
    CHECK(t.received == 990);
    CHECK(t.lost == 10);
    CHECK_NEAR(t.loss_pct, 1.0, 1e-9);
}

void test_seq_wrap_without_loss() {
    StreamStats s("audio");
    for (uint32_t i = 0; i < 20; ++i)
        feed(s, static_cast<uint16_t>(65530 + i), i);  // ... 65535, 0 .. 13
    const auto t = s.total();
    CHECK(t.expected == 20);
    CHECK(t.received == 20);
    CHECK(t.lost == 0);
}

void test_seq_wrap_with_loss() {
    StreamStats s("audio");
    for (uint32_t i = 0; i < 20; ++i) {
        const uint16_t seq = static_cast<uint16_t>(65530 + i);
        if (seq == 65535 || seq == 0) continue;  // lose the two packets around the wrap
        feed(s, seq, i);
    }
    const auto t = s.total();
    CHECK(t.expected == 20);
    CHECK(t.lost == 2);
}

void test_long_run_many_wraps() {
    StreamStats s("video");
    uint64_t dropped = 0;
    // Three wraps past 65535. Drop mid-run packets only: a loss at the very end of a run
    // cannot be seen, because no later packet reveals the gap.
    for (uint32_t i = 0; i < 200000; ++i) {
        if (i % 1000 == 500) {
            ++dropped;
            continue;
        }
        feed(s, static_cast<uint16_t>(i), i);
    }
    const auto t = s.total();
    CHECK(t.expected == 200000);
    CHECK(t.lost == dropped);
}

void test_reordering() {
    StreamStats s("audio");
    const uint16_t order[] = {0, 1, 3, 2, 4, 5};
    for (uint32_t i = 0; i < 6; ++i) feed(s, order[i], order[i]);
    const auto t = s.total();
    CHECK(t.expected == 6);
    CHECK(t.received == 6);
    CHECK(t.lost == 0);
    CHECK(t.reordered == 1);
}

void test_reordering_across_wrap() {
    StreamStats s("audio");
    const uint16_t order[] = {65533, 65534, 0, 65535, 1, 2};
    for (uint32_t i = 0; i < 6; ++i) feed(s, order[i], i);
    const auto t = s.total();
    CHECK(t.expected == 6);
    CHECK(t.lost == 0);
    CHECK(t.reordered == 1);
}

void test_duplicate_does_not_hide_loss() {
    StreamStats s("audio");
    // 0..9 with 5 delivered twice and 7 lost: a naive "received count" would show no loss.
    const uint16_t arrivals[] = {0, 1, 2, 3, 4, 5, 5, 6, 8, 9};
    for (uint32_t i = 0; i < 10; ++i) feed(s, arrivals[i], arrivals[i]);
    const auto t = s.total();
    CHECK(t.expected == 10);
    CHECK(t.received == 9);
    CHECK(t.duplicates == 1);
    CHECK(t.lost == 1);
    CHECK_NEAR(t.loss_pct, 10.0, 1e-9);
}

void test_jitter_alternating_delay() {
    StreamStats s("audio");
    // Network delay alternates 5 ms / 15 ms, so |D| = 10 ms every packet -> J converges to 10 ms.
    for (uint32_t i = 0; i < 300; ++i) feed(s, static_cast<uint16_t>(i), i, (i % 2) ? 10000 : 0);
    CHECK_NEAR(s.total().jitter_ms, 10.0, 0.01);
}

void test_jitter_ignores_clock_offset() {
    StreamStats s("audio");
    // Receiver clock far ahead of the sender's (and wrapping past 2^32): only differences matter.
    for (uint32_t i = 0; i < 100; ++i) {
        const uint32_t ts = i * kStep;
        s.on_packet(static_cast<uint16_t>(i), ts, kBytes, ts + 0xFFFF0000u);
    }
    CHECK_NEAR(s.total().jitter_ms, 0.0, 1e-12);
}

void test_rates_and_intervals() {
    StreamStats s("video");
    const uint32_t t0 = 0xFFFFFFFFu - 500000;  // window crosses the 32-bit time wrap
    // Second 1: 50 packets every 20 ms, 15 frames.
    for (uint32_t i = 0; i < 50; ++i)
        s.on_packet(static_cast<uint16_t>(i), i * kStep, kBytes, t0 + i * kStep);
    for (uint32_t f = 0; f < 15; ++f) s.on_frame(t0 + f * 66000);
    auto a = s.take_interval(t0 + 1000000);
    CHECK_NEAR(a.window_s, 1.0, 1e-9);
    CHECK_NEAR(a.pps, 50.0, 1e-9);
    CHECK_NEAR(a.kbps, 50 * kBytes * 8 / 1000.0, 1e-9);  // 260.8 kbps
    CHECK_NEAR(a.fps, 15.0, 1e-9);
    CHECK(a.expected == 50);
    CHECK(a.lost == 0);

    // Second 2: seqs 50..99 with 5 lost; interval counters must start fresh.
    for (uint32_t i = 50; i < 100; ++i) {
        if (i % 10 == 3) continue;
        s.on_packet(static_cast<uint16_t>(i), i * kStep, kBytes, t0 + i * kStep);
    }
    auto b = s.take_interval(t0 + 2000000);
    CHECK(b.expected == 50);
    CHECK(b.received == 45);
    CHECK(b.lost == 5);
    CHECK_NEAR(b.loss_pct, 10.0, 1e-9);
    CHECK_NEAR(b.pps, 45.0, 1e-9);
    CHECK(b.frames == 0);

    const auto t = s.total();
    CHECK(t.expected == 100);
    CHECK(t.lost == 5);
    CHECK_NEAR(t.loss_pct, 5.0, 1e-9);

    // Empty interval: nothing arrived.
    auto c = s.take_interval(t0 + 3000000);
    CHECK(c.expected == 0);
    CHECK(c.pps == 0.0);
}

void test_single_stray_jump_is_ignored() {
    StreamStats s("audio");
    for (uint32_t i = 0; i < 100; ++i) feed(s, static_cast<uint16_t>(i), i);
    feed(s, 30000, 100);  // one corrupt / stray packet far ahead
    for (uint32_t i = 100; i < 200; ++i) feed(s, static_cast<uint16_t>(i), i);
    const auto t = s.total();
    CHECK(t.expected == 200);
    CHECK(t.lost == 0);
    CHECK(t.restarts == 0);
}

void test_sender_restart() {
    StreamStats s("audio");
    for (uint32_t i = 0; i < 100; ++i) feed(s, static_cast<uint16_t>(i), i);
    // Sender restarts with a new sequence space: 5000, 5001, ...
    for (uint32_t i = 0; i < 100; ++i) feed(s, static_cast<uint16_t>(5000 + i), 100 + i);
    const auto t = s.total();
    CHECK(t.restarts == 1);
    // RFC 3550: the first packet of the new run is used only to confirm the restart.
    CHECK(t.expected == 99);
    CHECK(t.received == 99);
    CHECK(t.lost == 0);
}

void test_no_packets() {
    StreamStats s("audio");
    s.on_frame(1000);  // ignored: no packet yet
    const auto t = s.total();
    CHECK(t.frames == 0);
    CHECK(t.expected == 0);
    CHECK(t.loss_pct == 0.0);
    CHECK(s.take_interval(123).expected == 0);
}

void test_format() {
    StreamStats s("video");
    for (uint32_t i = 0; i < 10; ++i) feed(s, static_cast<uint16_t>(i), i);
    s.on_frame(5000);
    const std::string line = s.format(s.total());
    CHECK(line.rfind("video", 0) == 0);
    CHECK(line.find("loss") != std::string::npos);
    CHECK(line.find("jitter") != std::string::npos);
    CHECK(line.find("fps") != std::string::npos);
}

void test_concurrent_use() {
    StreamStats s("video");
    feed(s, 0, 0);  // first packet before the threads start, so every on_frame() counts
    std::thread rx([&] {
        for (uint32_t i = 1; i < 20000; ++i) feed(s, static_cast<uint16_t>(i), i);
    });
    std::thread ui([&] {
        for (uint32_t i = 0; i < 20000; ++i) {
            s.on_frame(i * kStep);
            if (i % 100 == 0) s.take_interval(i * kStep);
        }
    });
    rx.join();
    ui.join();
    const auto t = s.total();
    CHECK(t.received == 20000);
    CHECK(t.lost == 0);
    CHECK(t.frames == 20000);
}

}  // namespace

int main() {
    RUN(test_no_loss);
    RUN(test_known_loss);
    RUN(test_seq_wrap_without_loss);
    RUN(test_seq_wrap_with_loss);
    RUN(test_long_run_many_wraps);
    RUN(test_reordering);
    RUN(test_reordering_across_wrap);
    RUN(test_duplicate_does_not_hide_loss);
    RUN(test_jitter_alternating_delay);
    RUN(test_jitter_ignores_clock_offset);
    RUN(test_rates_and_intervals);
    RUN(test_single_stray_jump_is_ignored);
    RUN(test_sender_restart);
    RUN(test_no_packets);
    RUN(test_format);
    RUN(test_concurrent_use);
    return check::result();
}
