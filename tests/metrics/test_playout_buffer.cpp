// Unit tests for audio::PlayoutBuffer (the audio receiver's jitter buffer).
// Lives in tests/metrics/ because tests/audio/ is not part of the shared test layout.
//
// Each test chunk is filled with one value derived from its seq, so the chunk that comes
// out of next_chunk() tells us exactly which packet was played (0 = silence).

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "audio/playout_buffer.hpp"
#include "check.hpp"

using audio::kChunkSamples;
using audio::PlayoutBuffer;
using Insert = PlayoutBuffer::Insert;

namespace {

using Chunk = std::array<int16_t, kChunkSamples>;

// Never 0, so a played packet is always distinguishable from silence.
int16_t tag_value(uint16_t seq) {
    return static_cast<int16_t>((seq % 30000) + 1);
}

// Capture time a test packet carries: 1 s + 20 ms per sequence number (wraps mod 2^32).
uint32_t capture_of(uint16_t seq) {
    return 1000000u + static_cast<uint32_t>(seq) * audio::kChunkUs;
}

Insert put(PlayoutBuffer& b, uint16_t seq, uint32_t capture_us) {
    Chunk c;
    c.fill(tag_value(seq));
    return b.insert(seq, capture_us, c.data());
}

Insert put(PlayoutBuffer& b, uint16_t seq) {
    return put(b, seq, capture_of(seq));
}

// Pulls one chunk and also returns its timing.
int pull_timed(PlayoutBuffer& b, audio::ChunkTime& t) {
    Chunk c;
    c.fill(-7);
    b.next_chunk(c.data(), t);
    for (int16_t v : c) {
        if (v != c[0]) return -1;
    }
    return c[0];
}

// Pulls one chunk; returns the value it was filled with (0 = silence), or -1 if the chunk
// was not uniform (a torn copy).
int pull(PlayoutBuffer& b) {
    Chunk c;
    c.fill(-7);
    b.next_chunk(c.data());
    for (int16_t v : c) {
        if (v != c[0]) return -1;
    }
    return c[0];
}

void test_timing_of_played_chunks() {
    PlayoutBuffer b(3, 8);
    audio::ChunkTime t;
    CHECK(pull_timed(b, t) == 0);  // idle: no time
    CHECK(!t.valid);
    for (uint16_t s = 0; s < 3; ++s) put(b, s);
    for (uint16_t s = 0; s < 3; ++s) {
        CHECK(pull_timed(b, t) == tag_value(s));
        CHECK(t.valid);
        CHECK(t.capture_us == capture_of(s));
    }
    CHECK(pull_timed(b, t) == 0);  // underrun: no time
    CHECK(!t.valid);
    put(b, 3);
    CHECK(pull_timed(b, t) == 0);  // re-buffering: no time
    CHECK(!t.valid);
}

void test_timing_of_concealed_chunks() {
    PlayoutBuffer b(3, 8);
    // 2 and 3 are lost; the capture times of 4 and 5 deliberately don't follow 20 ms steps,
    // to check that concealment extrapolates from the last PLAYED chunk.
    put(b, 0);
    put(b, 1);
    put(b, 4, capture_of(4) + 777);
    put(b, 5, capture_of(5) + 777);
    audio::ChunkTime t;
    CHECK(pull_timed(b, t) == tag_value(0));
    CHECK(pull_timed(b, t) == tag_value(1));
    CHECK(pull_timed(b, t) == 0);  // 2 concealed
    CHECK(t.valid);
    CHECK(t.capture_us == capture_of(1) + 1 * audio::kChunkUs);
    CHECK(pull_timed(b, t) == 0);  // 3 concealed
    CHECK(t.valid);
    CHECK(t.capture_us == capture_of(1) + 2 * audio::kChunkUs);
    CHECK(pull_timed(b, t) == tag_value(4));  // real timestamp wins again
    CHECK(t.capture_us == capture_of(4) + 777);
    CHECK(b.counters().concealed == 2);
}

void test_timing_across_capture_clock_wrap() {
    PlayoutBuffer b(3, 8);
    const uint32_t base = 0xFFFFFFFFu - 30000;  // sender clock wraps between chunk 1 and 2
    put(b, 0, base);
    put(b, 1, base + audio::kChunkUs);
    put(b, 3, base + 3 * audio::kChunkUs);  // 2 lost
    audio::ChunkTime t;
    pull_timed(b, t);
    pull_timed(b, t);
    CHECK(pull_timed(b, t) == 0);  // 2 concealed, its time is past the wrap
    CHECK(t.valid);
    CHECK(t.capture_us == static_cast<uint32_t>(base + 2 * audio::kChunkUs));
    CHECK(t.capture_us < 100000u);
}

void test_no_concealment_time_right_after_resync() {
    PlayoutBuffer b(3, 8);
    for (uint16_t s = 30000; s < 30005; ++s) {
        put(b, s);
        pull(b);
    }
    put(b, 0, 5000000);  // sender restarted: new seqs and a new timeline
    pull(b);             // consumer resyncs and waits for seq 1 onwards
    put(b, 2, 5040000);  // 1 lost
    put(b, 3, 5060000);
    audio::ChunkTime t;
    CHECK(pull_timed(b, t) == 0);  // 1 concealed before anything played on the new timeline:
    CHECK(!t.valid);               // no time rather than one made up from the old timeline
    CHECK(pull_timed(b, t) == tag_value(2));
    CHECK(t.valid);
    CHECK(t.capture_us == 5040000u);
    CHECK(b.counters().resyncs == 1);
}

void test_idle_before_first_packet() {
    PlayoutBuffer b(3, 8);
    CHECK(pull(b) == 0);
    CHECK(pull(b) == 0);
    const auto c = b.counters();
    CHECK(c.idle == 2);
    CHECK(c.underruns == 0);
}

void test_prebuffer_then_in_order() {
    PlayoutBuffer b(3, 8);
    CHECK(put(b, 0) == Insert::Stored);
    CHECK(put(b, 1) == Insert::Stored);
    CHECK(pull(b) == 0);  // only 2 of 3 queued: still buffering
    CHECK(put(b, 2) == Insert::Stored);
    CHECK(pull(b) == tag_value(0));
    CHECK(pull(b) == tag_value(1));
    CHECK(pull(b) == tag_value(2));
    const auto c = b.counters();
    CHECK(c.played == 3);
    CHECK(c.buffering == 1);
    CHECK(c.concealed == 0);
}

void test_underrun_then_rebuffer() {
    PlayoutBuffer b(3, 8);
    for (uint16_t s = 0; s < 3; ++s) put(b, s);
    for (int i = 0; i < 3; ++i) pull(b);
    CHECK(pull(b) == 0);  // nothing left: underrun
    CHECK(b.counters().underruns == 1);
    put(b, 3);
    put(b, 4);
    CHECK(pull(b) == 0);  // re-buffering: 2 of 3
    put(b, 5);
    CHECK(pull(b) == tag_value(3));  // delayed packets still play, nothing skipped
    CHECK(pull(b) == tag_value(4));
    CHECK(pull(b) == tag_value(5));
    const auto c = b.counters();
    CHECK(c.underruns == 1);
    CHECK(c.concealed == 0);
    CHECK(c.late == 0);
}

void test_reordering_inside_buffer() {
    PlayoutBuffer b(3, 8);
    put(b, 0);
    put(b, 2);
    put(b, 1);
    CHECK(pull(b) == tag_value(0));
    CHECK(pull(b) == tag_value(1));
    CHECK(pull(b) == tag_value(2));
    CHECK(b.counters().concealed == 0);
}

void test_missing_chunk_is_concealed() {
    PlayoutBuffer b(3, 8);
    for (uint16_t s : {0, 1, 3, 4}) put(b, s);
    CHECK(pull(b) == tag_value(0));
    CHECK(pull(b) == tag_value(1));
    CHECK(pull(b) == 0);  // 2 never arrived
    CHECK(pull(b) == tag_value(3));
    CHECK(pull(b) == tag_value(4));
    const auto c = b.counters();
    CHECK(c.concealed == 1);
    CHECK(c.played == 4);
    CHECK(c.underruns == 0);
}

void test_late_and_duplicate() {
    PlayoutBuffer b(3, 8);
    for (uint16_t s = 0; s < 4; ++s) put(b, s);
    CHECK(put(b, 3) == Insert::Duplicate);
    pull(b);
    pull(b);  // played 0 and 1
    CHECK(put(b, 1) == Insert::Late);
    CHECK(put(b, 0) == Insert::Late);
    const auto c = b.counters();
    CHECK(c.late == 2);
    CHECK(c.duplicates == 1);
    CHECK(pull(b) == tag_value(2));
}

void test_trim_keeps_delay_bounded() {
    PlayoutBuffer b(3, 8);
    for (uint16_t s = 0; s < 20; ++s) put(b, s);  // 400 ms queued at once
    CHECK(pull(b) == tag_value(17));              // skipped to newest - 2
    CHECK(pull(b) == tag_value(18));
    CHECK(pull(b) == tag_value(19));
    const auto c = b.counters();
    CHECK(c.trimmed == 17);
    CHECK(c.played == 3);
}

void test_slow_drift_is_trimmed() {
    // Sender produces slightly faster than we play: one extra chunk every 10.
    PlayoutBuffer b(3, 8);
    uint16_t seq = 0;
    for (int i = 0; i < 3; ++i) put(b, seq++);
    int max_depth = 0;
    for (int tick = 0; tick < 2000; ++tick) {
        put(b, seq++);
        if (tick % 10 == 0) put(b, seq++);
        pull(b);
        if (b.depth_chunks() > max_depth) max_depth = b.depth_chunks();
    }
    CHECK(max_depth <= 8);
    CHECK(b.counters().trimmed > 0);
    CHECK(b.counters().underruns == 0);
}

void test_one_second_outage() {
    PlayoutBuffer b(3, 8);
    uint16_t seq = 0;
    for (int i = 0; i < 3; ++i) put(b, seq++);
    for (int i = 0; i < 100; ++i) {  // 2 s of normal flow
        put(b, seq++);
        pull(b);
    }
    // 1 s outage: 50 packets lost while the speaker keeps asking for audio.
    const uint16_t resume = static_cast<uint16_t>(seq + 50);
    for (int i = 0; i < 50; ++i) pull(b);
    CHECK(b.counters().underruns == 1);
    for (uint16_t s = resume; s < resume + 3; ++s) CHECK(put(b, s) == Insert::Stored);
    CHECK(pull(b) == tag_value(resume));  // trimmed straight to the fresh packets
    CHECK(b.counters().out_of_range == 0);
}

void test_long_outage_resyncs() {
    PlayoutBuffer b(3, 8);
    for (uint16_t s = 0; s < 3; ++s) put(b, s);
    for (int i = 0; i < 3; ++i) pull(b);
    // 3 s outage: next packet is 150 ahead, beyond the 64-slot buffer.
    CHECK(put(b, 153) == Insert::OutOfRange);
    pull(b);  // consumer jumps
    CHECK(b.counters().resyncs == 1);
    for (uint16_t s = 154; s < 157; ++s) CHECK(put(b, s) == Insert::Stored);
    CHECK(pull(b) == tag_value(154));
    CHECK(pull(b) == tag_value(155));
}

void test_sender_restart_resyncs() {
    PlayoutBuffer b(3, 8);
    for (uint16_t s = 30000; s < 30010; ++s) {
        put(b, s);
        pull(b);
    }
    // Sender restarts at seq 0: far "behind" the play position.
    CHECK(put(b, 0) == Insert::OutOfRange);
    pull(b);
    CHECK(b.counters().resyncs == 1);
    for (uint16_t s = 1; s < 4; ++s) CHECK(put(b, s) == Insert::Stored);
    CHECK(pull(b) == tag_value(1));
    CHECK(pull(b) == tag_value(2));
    CHECK(pull(b) == tag_value(3));
}

void test_seq_wrap() {
    PlayoutBuffer b(3, 8);
    std::vector<uint16_t> seqs;
    for (uint32_t s = 65530; s < 65530 + 12; ++s) seqs.push_back(static_cast<uint16_t>(s));
    for (int i = 0; i < 3; ++i) put(b, seqs[i]);
    for (size_t i = 3; i < seqs.size(); ++i) {
        put(b, seqs[i]);
        CHECK(pull(b) == tag_value(seqs[i - 3]));
    }
    const auto c = b.counters();
    CHECK(c.concealed == 0);
    CHECK(c.late == 0);
    CHECK(c.out_of_range == 0);
}

void test_two_threads_stress() {
    // Producer delivers 0..N-1 with some losses and swaps; consumer pulls as fast as it can.
    // Every chunk played must be whole (never torn) and in increasing seq order.
    PlayoutBuffer b(3, 8);
    constexpr uint16_t kCount = 29000;
    std::atomic<bool> done{false};
    bool order_ok = true, whole_ok = true;
    int played = 0;

    std::thread consumer([&] {
        int last = 0;
        while (!done.load() || b.depth_chunks() > 0) {
            const int v = pull(b);
            if (v < 0) whole_ok = false;
            if (v > 0) {
                if (v <= last) order_ok = false;
                last = v;
                ++played;
            }
            if (done.load() && b.depth_chunks() == 0) break;
        }
    });
    std::thread producer([&] {
        for (uint16_t s = 0; s < kCount; ++s) {
            if (s % 97 == 0) continue;            // lost
            if (s % 31 == 0 && s + 1 < kCount) {  // swap with the next one
                put(b, static_cast<uint16_t>(s + 1));
                put(b, s);
                ++s;
                continue;
            }
            put(b, s);
            if (s % 64 == 0) std::this_thread::yield();
        }
        done.store(true);
    });
    producer.join();
    consumer.join();
    CHECK(order_ok);
    CHECK(whole_ok);
    CHECK(played > 0);
}

}  // namespace

int main() {
    RUN(test_timing_of_played_chunks);
    RUN(test_timing_of_concealed_chunks);
    RUN(test_timing_across_capture_clock_wrap);
    RUN(test_no_concealment_time_right_after_resync);
    RUN(test_idle_before_first_packet);
    RUN(test_prebuffer_then_in_order);
    RUN(test_underrun_then_rebuffer);
    RUN(test_reordering_inside_buffer);
    RUN(test_missing_chunk_is_concealed);
    RUN(test_late_and_duplicate);
    RUN(test_trim_keeps_delay_bounded);
    RUN(test_slow_drift_is_trimmed);
    RUN(test_one_second_outage);
    RUN(test_long_outage_resyncs);
    RUN(test_sender_restart_resyncs);
    RUN(test_seq_wrap);
    RUN(test_two_threads_stress);
    return check::result();
}
