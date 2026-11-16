#pragma once
// PlayoutBuffer — the audio receiver's jitter buffer (Person 2, audio/).
//
// The network thread inserts 20 ms chunks by sequence number; the speaker callback takes
// them out in order through ChunkSource::next_chunk(). No locks: one producer thread, one
// consumer (the audio callback), 64 slots (1.28 s) indexed by seq % 64.
//
// Playback rules (consumer side):
//   waiting    no packet yet: play silence (counted as idle, not as an underrun)
//   buffering  wait until `prebuffer` chunks are queued (default 3 = 60 ms), playing silence
//   playing    next chunk present            -> play it                         (played)
//              missing, newer ones queued    -> 20 ms of silence, move on       (concealed)
//              nothing queued at all         -> silence, back to buffering      (underruns)
//   at any time, if more than `max_chunks` are queued (sound-card clock drift, a burst),
//   skip ahead so only `prebuffer` remain (trimmed); if the gap is bigger than the buffer
//   itself (a long outage), jump to the newest packets (resyncs).
// Producer side: packets behind the play position are dropped (late), repeats are ignored
// (duplicates). A packet 64+ chunks ahead, or more than 64 behind (the sender restarted with
// a new sequence space), cannot be stored (out_of_range) and makes the consumer jump to it
// (resyncs).
//
// next_chunk() always fills the chunk (silence where needed) and returns true, so Playback's
// own underrun counter stays 0; this class's counters are the ones to report.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "audio/pa_audio.hpp"

namespace audio {

class PlayoutBuffer : public ChunkSource {
public:
    static constexpr int kSlots = 64;

    enum class Insert { Stored, Late, Duplicate, OutOfRange };

    // prebuffer: chunks queued before playback starts (>= 1).
    // max_chunks: queue depth above which playback skips ahead (> prebuffer, < kSlots).
    explicit PlayoutBuffer(int prebuffer = 3, int max_chunks = 8);

    // Network thread only. samples: kChunkSamples host-order int16 samples.
    Insert insert(uint16_t seq, const int16_t* samples);

    // Audio callback only. Always fills kChunkSamples samples and returns true.
    bool next_chunk(int16_t* out) override;

    struct Counters {
        uint64_t played = 0;        // chunks played from received packets
        uint64_t concealed = 0;     // missing chunks replaced by silence
        uint64_t underruns = 0;     // times the buffer ran empty (then re-buffers)
        uint64_t buffering = 0;     // silent chunks played while (re)buffering
        uint64_t idle = 0;          // silent chunks before the first packet
        uint64_t late = 0;          // packets dropped: arrived after their play time
        uint64_t duplicates = 0;    // packets dropped: already queued
        uint64_t out_of_range = 0;  // packets dropped: 64+ ahead or 64+ behind the play position
        uint64_t trimmed = 0;       // chunks skipped to keep the delay down
        uint64_t resyncs = 0;       // jumps to the newest packets (long gap or sender restart)
    };
    Counters counters() const;  // any thread

    // Chunks from the play position to the newest packet (approximate; any thread).
    int depth_chunks() const;
    int prebuffer() const { return prebuffer_; }
    int max_chunks() const { return max_chunks_; }

private:
    static constexpr uint32_t kEmpty = 0x10000;  // never a valid uint16 seq

    struct Slot {
        std::atomic<uint32_t> tag{kEmpty};  // seq stored here, or kEmpty
        std::array<int16_t, kChunkSamples> samples{};
    };

    enum class State { Waiting, Buffering, Playing };

    static int seq_diff(uint16_t a, uint16_t b) {
        return static_cast<int16_t>(static_cast<uint16_t>(a - b));
    }
    void skip_to(uint16_t new_head);  // consumer only
    static void bump(std::atomic<uint64_t>& c, uint64_t n = 1) {
        c.fetch_add(n, std::memory_order_relaxed);
    }

    const int prebuffer_;
    const int max_chunks_;
    std::array<Slot, kSlots> slots_;

    // Shared between the threads.
    std::atomic<bool> started_{false};   // producer: first packet seen
    std::atomic<uint16_t> newest_{0};    // producer: highest seq seen (or the jump target)
    std::atomic<bool> resync_{false};    // producer asks the consumer to jump to newest_
    std::atomic<uint16_t> head_pub_{0};  // consumer: next seq to play (producer reads it)

    // Consumer (audio callback) only.
    State state_ = State::Waiting;
    uint16_t head_ = 0;

    std::atomic<uint64_t> played_{0}, concealed_{0}, underruns_{0}, buffering_{0}, idle_{0};
    std::atomic<uint64_t> late_{0}, duplicates_{0}, out_of_range_{0}, trimmed_{0}, resyncs_{0};
};

}  // namespace audio
