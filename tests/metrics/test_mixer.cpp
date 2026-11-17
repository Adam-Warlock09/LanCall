// Unit tests for audio/mixer.hpp: mixing several streams in the speaker callback, and the
// audio clock per stream (phase 4 group calls). No sound card needed: the tests call
// next_chunk() and on_scheduled() the way Playback's callback does.
// Lives in tests/metrics/ because tests/audio/ is not part of the shared test layout.

#include "audio/mixer.hpp"

#include <array>
#include <cstdint>
#include <vector>

#include "check.hpp"

using audio::kChunkSamples;
using audio::kChunkUs;
using audio::StreamMixer;
using Insert = audio::PlayoutBuffer::Insert;

namespace {

using Chunk = std::array<int16_t, kChunkSamples>;

// Each sender has its own capture clock: stream s starts at a different base.
uint32_t capture_of(uint8_t stream, uint16_t seq) {
    return 1000000u * (stream + 1) * 37u + seq * kChunkUs;
}

Insert put(StreamMixer& m, uint8_t stream, uint16_t seq, int16_t value) {
    Chunk c;
    c.fill(value);
    return m.insert(stream, seq, capture_of(stream, seq), c.data());
}

// One callback step: pull a chunk, then tell the mixer when it plays. Returns the first
// sample (every test chunk is uniform) and the chunk's time.
int16_t step(StreamMixer& m, uint32_t play_at, audio::ChunkTime& t) {
    Chunk out;
    out.fill(-1);
    m.next_chunk(out.data(), t);
    m.on_scheduled(play_at);
    for (int16_t v : out) {
        if (v != out[0]) return -9999;  // not uniform: a broken mix
    }
    return out[0];
}

int16_t step(StreamMixer& m, uint32_t play_at) {
    audio::ChunkTime t;
    return step(m, play_at, t);
}

void test_mix_add_and_clip() {
    audio::MixAccumulator acc{};
    Chunk a, b, out;
    a.fill(20000);
    b.fill(20000);
    audio::mix_add(acc, a.data());
    audio::mix_out(acc, out.data());
    CHECK(out[0] == 20000);  // one stream: unchanged
    audio::mix_add(acc, b.data());
    audio::mix_out(acc, out.data());
    CHECK(out[0] == 32767);  // 40000 clipped
    acc.fill(0);
    a.fill(-20000);
    audio::mix_add(acc, a.data());
    audio::mix_add(acc, a.data());
    audio::mix_out(acc, out.data());
    CHECK(out[kChunkSamples - 1] == -32768);  // -40000 clipped
    acc.fill(0);
    a.fill(1000);
    b.fill(-300);
    audio::mix_add(acc, a.data());
    audio::mix_add(acc, b.data());
    audio::mix_out(acc, out.data());
    CHECK(out[5] == 700);
}

void test_silence_before_any_packet() {
    StreamMixer m(3, 8);
    audio::ChunkTime t;
    CHECK(step(m, 5000, t) == 0);
    CHECK(!t.valid);
    CHECK(m.active_streams(5000).empty());
    uint32_t c = 0;
    CHECK(!m.playing_capture_us(0, 5000, c));
    CHECK(!m.in_use(0));
}

void test_one_stream_passes_through() {
    // stream 0 = a two-person call: the mixer must behave like the plain playout buffer
    StreamMixer m(3, 8);
    for (uint16_t s = 0; s < 3; ++s) put(m, 0, s, static_cast<int16_t>(100 + s));
    audio::ChunkTime t;
    CHECK(step(m, 50000, t) == 100);
    CHECK(t.valid);
    CHECK(t.capture_us == capture_of(0, 0));
    uint32_t c = 0;
    CHECK(m.playing_capture_us(0, 50000, c));
    CHECK(c == capture_of(0, 0));
    CHECK(m.playing_capture_us(0, 55000, c));  // 5 ms into the chunk
    CHECK(c == capture_of(0, 0) + 5000);
    CHECK(step(m, 70000) == 101);
    CHECK(m.counters(0).played == 2);
}

void test_two_streams_mixed_with_own_clocks() {
    StreamMixer m(3, 8);
    for (uint16_t s = 0; s < 3; ++s) {
        put(m, 1, s, 100);
        put(m, 2, s, 250);
    }
    audio::ChunkTime t;
    CHECK(step(m, 80000, t) == 350);
    CHECK(t.valid);
    CHECK(t.capture_us == capture_of(1, 0));  // Playback's single clock: the lowest id
    uint32_t c1 = 0, c2 = 0;
    CHECK(m.playing_capture_us(1, 80000, c1));
    CHECK(m.playing_capture_us(2, 80000, c2));
    CHECK(c1 == capture_of(1, 0));
    CHECK(c2 == capture_of(2, 0));  // its own sender's clock, not stream 1's
    CHECK(m.active_streams(80000) == (std::vector<uint8_t>{1, 2}));
    uint32_t c = 0;
    CHECK(!m.playing_capture_us(3, 80000, c));  // never heard
}

void test_loud_streams_clip() {
    StreamMixer m(3, 8);
    for (uint16_t s = 0; s < 3; ++s) {
        put(m, 1, s, 30000);
        put(m, 2, s, 30000);
        put(m, 3, s, -10000);
    }
    CHECK(step(m, 1000) == 32767);  // 50000 clipped, no wrap-around to a negative value
    StreamMixer n(3, 8);
    for (uint16_t s = 0; s < 3; ++s) {
        put(n, 4, s, -30000);
        put(n, 5, s, -30000);
    }
    CHECK(step(n, 1000) == -32768);
}

void test_one_stream_stops() {
    // member 1 goes quiet (left, or a long outage); member 2 keeps talking
    StreamMixer m(3, 8);
    uint16_t seq = 0;
    for (; seq < 3; ++seq) {
        put(m, 1, seq, 100);
        put(m, 2, seq, 250);
    }
    uint32_t play_at = 100000;
    CHECK(step(m, play_at) == 350);
    for (int i = 0; i < 3; ++i) {  // 1 has two queued chunks left, then runs dry
        put(m, 2, seq++, 250);
        play_at += kChunkUs;
        step(m, play_at);
    }
    audio::ChunkTime t;
    put(m, 2, seq++, 250);
    play_at += kChunkUs;
    CHECK(step(m, play_at, t) == 250);        // only 2 is heard now
    CHECK(t.capture_us == capture_of(2, 4));  // the single clock moved to the stream with sound
    uint32_t c = 0;
    CHECK(!m.playing_capture_us(1, play_at, c));  // silence: no clock for 1, show its video as is
    CHECK(m.playing_capture_us(2, play_at, c));
    CHECK(m.counters(1).underruns == 1);
    CHECK(m.active_streams(play_at) == (std::vector<uint8_t>{1, 2}));  // 1 was heard < 2 s ago

    // 2.5 s later, 2 still talking: 1 is no longer active
    for (int i = 0; i < 125; ++i) {
        put(m, 2, seq++, 250);
        play_at += kChunkUs;
        step(m, play_at);
    }
    CHECK(m.active_streams(play_at) == (std::vector<uint8_t>{2}));
}

void test_bad_stream_id() {
    StreamMixer m(3, 8);
    Chunk c;
    c.fill(5);
    CHECK(!StreamMixer::valid_stream(StreamMixer::kStreams));
    CHECK(m.insert(StreamMixer::kStreams, 0, 0, c.data()) == Insert::OutOfRange);
    CHECK(m.insert(200, 0, 0, c.data()) == Insert::OutOfRange);
    CHECK(!m.in_use(200));
    CHECK(m.counters(200).played == 0);
    CHECK(m.depth_chunks(200) == 0);
    uint32_t t = 0;
    CHECK(!m.playing_capture_us(200, 0, t));
    CHECK(step(m, 0) == 0);  // nothing was stored anywhere
}

void test_clock_across_wrap() {
    // the local clock wraps between the chunk's start and the query
    StreamMixer m(3, 8);
    for (uint16_t s = 0; s < 3; ++s) put(m, 1, s, 1);
    const uint32_t play_at = 0xFFFFFFFFu - 2000;
    step(m, play_at);
    uint32_t c = 0;
    CHECK(m.playing_capture_us(1, play_at + 7000, c));  // 7 ms later, past the wrap
    CHECK(c == capture_of(1, 0) + 7000);
    CHECK(m.active_streams(play_at + 7000) == (std::vector<uint8_t>{1}));
}

}  // namespace

int main() {
    RUN(test_mix_add_and_clip);
    RUN(test_silence_before_any_packet);
    RUN(test_one_stream_passes_through);
    RUN(test_two_streams_mixed_with_own_clocks);
    RUN(test_loud_streams_clip);
    RUN(test_one_stream_stops);
    RUN(test_bad_stream_id);
    RUN(test_clock_across_wrap);
    return check::result();
}
