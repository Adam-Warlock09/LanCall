// Unit tests for call/lip_sync.hpp (which video frame to show for the audio being heard).
// Lives in tests/metrics/ because Person 2's tests stay in one folder (no tests/call/).

#include "call/lip_sync.hpp"

#include <cstdint>
#include <memory>
#include <optional>

#include "check.hpp"

using call::LipSync;
using call::LipSyncConfig;

namespace {

constexpr uint32_t kMs = 1000;

// Frames are numbered by their capture time in ms, so the returned value tells which one it was.
void push_ms(LipSync<int>& s, uint32_t capture_ms, uint32_t arrival_us = 0) {
    s.push(static_cast<int>(capture_ms), capture_ms * kMs, arrival_us);
}

void test_empty_queue() {
    LipSync<int> s;
    int f = -1;
    uint32_t ts = 0;
    CHECK(!s.select(std::nullopt, 0, f, ts));
    CHECK(!s.select(1000u, 0, f, ts));
    CHECK(f == -1);
}

void test_no_audio_shows_newest() {
    LipSync<int> s;
    push_ms(s, 0);
    push_ms(s, 66);
    push_ms(s, 133);
    int f = -1;
    uint32_t ts = 0;
    CHECK(s.select(std::nullopt, 0, f, ts));
    CHECK(f == 133);
    CHECK(ts == 133 * kMs);
    CHECK(s.queued() == 0);
    CHECK(s.counters().no_audio == 1);
    CHECK(s.counters().skipped == 2);
    CHECK(s.counters().shown == 1);
}

void test_shows_newest_frame_not_after_audio() {
    LipSync<int> s;
    for (uint32_t ms : {0u, 66u, 133u, 200u}) push_ms(s, ms);
    int f = -1;
    uint32_t ts = 0;
    CHECK(s.select(150 * kMs, 0, f, ts));  // audio is at 150 ms: 133 is due, 0 and 66 are stale
    CHECK(f == 133);
    CHECK(s.counters().skipped == 2);
    CHECK(s.queued() == 1);
    CHECK(!s.select(150 * kMs, 1000, f, ts));  // 200 is ahead of the audio: wait
    CHECK(s.select(200 * kMs, 2000, f, ts));   // exactly equal counts as due
    CHECK(f == 200);
    CHECK(s.counters().forced == 0);
}

void test_waits_then_forces_after_max_wait() {
    LipSyncConfig cfg;
    cfg.max_wait_us = 500 * kMs;
    LipSync<int> s(cfg);
    push_ms(s, 1000, /*arrival_us=*/10 * kMs);
    int f = -1;
    uint32_t ts = 0;
    CHECK(!s.select(900 * kMs, 10 * kMs + 499 * kMs, f, ts));  // waited 499 ms: still waits
    CHECK(s.select(900 * kMs, 10 * kMs + 500 * kMs, f, ts));   // 500 ms: shown anyway
    CHECK(f == 1000);
    CHECK(s.counters().forced == 1);
}

void test_queue_bound_drops_oldest() {
    LipSyncConfig cfg;
    cfg.max_queue = 3;
    LipSync<int> s(cfg);
    for (uint32_t ms = 0; ms < 5; ++ms) push_ms(s, ms);
    CHECK(s.queued() == 3);
    CHECK(s.counters().dropped_full == 2);
    CHECK(s.counters().pushed == 5);
    int f = -1;
    uint32_t ts = 0;
    CHECK(s.select(2 * kMs, 0, f, ts));  // 0 and 1 were dropped; 2 is the oldest left
    CHECK(f == 2);
}

void test_wraps_past_2_pow_32() {
    LipSync<int> s;
    // Captures straddle the sender's clock wrap.
    s.push(1, 0xFFFFFF00u, 0);
    s.push(2, 0xFFFFFFF0u, 0);
    s.push(3, 0x00000010u, 0);
    s.push(4, 0x00000100u, 0);
    int f = -1;
    uint32_t ts = 0;
    CHECK(s.select(0x00000020u, 0, f, ts));  // audio just after the wrap
    CHECK(f == 3);
    CHECK(ts == 0x00000010u);
    CHECK(s.counters().skipped == 2);
    CHECK(s.queued() == 1);
}

void test_audio_jump_forward_skips_to_newest_due() {
    LipSync<int> s;
    for (uint32_t ms = 0; ms <= 1000; ms += 100) push_ms(s, ms);
    int f = -1;
    uint32_t ts = 0;
    CHECK(s.select(50 * kMs, 0, f, ts));
    CHECK(f == 0);
    CHECK(s.select(2000 * kMs, 0, f, ts));  // audio resynced 2 s ahead
    CHECK(f == 1000);
    CHECK(s.queued() == 0);
}

void test_video_ahead_then_audio_catches_up() {
    // Typical call: video arrives ~100 ms before matching audio is heard.
    LipSync<int> s;
    int shown_count = 0, last = -1;
    uint32_t next_frame_ms = 0;                 // 15 fps: a frame every 66 ms
    for (uint32_t t = 0; t <= 3000; t += 10) {  // 3 s, display loop every 10 ms
        if (t >= next_frame_ms) {               // frame captured at next_frame_ms arrives now
            push_ms(s, next_frame_ms, t * kMs);
            next_frame_ms += 66;
        }
        const uint32_t audio_ms = t >= 100 ? t - 100 : 0;  // audio heard 100 ms behind
        int f = -1;
        uint32_t ts = 0;
        if (t >= 100 && s.select(audio_ms * kMs, t * kMs, f, ts)) {
            CHECK(static_cast<uint32_t>(f) <= audio_ms);  // never shown before its audio
            CHECK(f > last);
            last = f;
            ++shown_count;
        }
    }
    CHECK(shown_count > 30);
    CHECK(s.counters().forced == 0);
}

void test_move_only_frames() {
    LipSync<std::unique_ptr<int>> s;
    s.push(std::make_unique<int>(7), 100, 0);
    std::unique_ptr<int> f;
    uint32_t ts = 0;
    CHECK(s.select(std::nullopt, 0, f, ts));
    CHECK(f && *f == 7);
}

}  // namespace

int main() {
    RUN(test_empty_queue);
    RUN(test_no_audio_shows_newest);
    RUN(test_shows_newest_frame_not_after_audio);
    RUN(test_waits_then_forces_after_max_wait);
    RUN(test_queue_bound_drops_oldest);
    RUN(test_wraps_past_2_pow_32);
    RUN(test_audio_jump_forward_skips_to_newest_due);
    RUN(test_video_ahead_then_audio_catches_up);
    RUN(test_move_only_frames);
    return check::result();
}
