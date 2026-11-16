// Unit tests for audio/audio_clock.hpp (the lip-sync audio clock) and RingSource timing.

#include "audio/audio_clock.hpp"

#include <cstdint>

#include "audio/pa_audio.hpp"
#include "check.hpp"

using audio::audio_clock_at;
using audio::kChunkUs;

namespace {

void test_during_the_chunk() {
    uint32_t out = 0;
    // Chunk captured at 5.000 s (sender clock) starts at the speaker at local 900.000 s.
    CHECK(audio_clock_at(5000000, 900000000, 900000000, out));
    CHECK(out == 5000000u);
    CHECK(audio_clock_at(5000000, 900000000, 900012345, out));  // 12.345 ms into the chunk
    CHECK(out == 5012345u);
    CHECK(audio_clock_at(5000000, 900000000, 900000000 + kChunkUs, out));  // end of chunk
    CHECK(out == 5000000u + kChunkUs);
}

void test_before_the_chunk_extrapolates_back() {
    // The newest chunk was handed over but only starts in 45 ms (output latency): what is
    // heard now is 45 ms earlier in the stream.
    uint32_t out = 0;
    CHECK(audio_clock_at(5000000, 900045000, 900000000, out));
    CHECK(out == 5000000u - 45000u);
}

void test_holds_after_the_chunk_then_goes_stale() {
    uint32_t out = 0;
    CHECK(audio_clock_at(5000000, 900000000, 900050000, out));   // next callback is late
    CHECK(out == 5000000u + kChunkUs);                           // hold at the chunk's end
    CHECK(audio_clock_at(5000000, 900000000, 900100000, out));   // exactly the stale limit
    CHECK(!audio_clock_at(5000000, 900000000, 900100001, out));  // playback stalled
}

void test_wraps() {
    uint32_t out = 0;
    // Both clocks wrap: local play time just before 2^32, sender capture just before 2^32.
    CHECK(audio_clock_at(0xFFFFFFF0u, 0xFFFFFF00u, 0x00000100u, out));  // 512 us into chunk
    CHECK(out == 0x000001F0u);                                          // 0xFFFFFFF0 + 512
    CHECK(audio_clock_at(0x00000010u, 0x00000100u, 0xFFFFFF00u, out));  // 512 us before start
    CHECK(out == 0xFFFFFE10u);
}

void test_ring_source_reports_capture_time() {
    audio::RingSource src;
    audio::AudioChunk c;
    c.capture_us = 4242;
    c.samples.fill(7);
    CHECK(src.push(c));
    int16_t out[audio::kChunkSamples] = {};
    audio::ChunkTime t;
    CHECK(src.next_chunk(out, t));
    CHECK(t.valid);
    CHECK(t.capture_us == 4242u);
    CHECK(out[0] == 7 && out[audio::kChunkSamples - 1] == 7);
    CHECK(!src.next_chunk(out));  // empty again; the one-argument form still works
}

}  // namespace

int main() {
    RUN(test_during_the_chunk);
    RUN(test_before_the_chunk_extrapolates_back);
    RUN(test_holds_after_the_chunk_then_goes_stale);
    RUN(test_wraps);
    RUN(test_ring_source_reports_capture_time);
    return check::result();
}
