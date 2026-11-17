#pragma once
// StreamMixer: the audio receiver's source when several people talk (Person 2, audio/,
// phase 4). One PlayoutBuffer per stream_id (= the sender's member id), mixed in the speaker
// callback, with an audio clock per stream for lip sync.
//
//   network thread   insert(stream_id, seq, capture_us, samples)   -> that stream's buffer
//   audio callback   next_chunk(): every stream in use gives one chunk (real, concealed or
//                    silence), the chunks are added up in 32 bits and clipped to int16
//                    on_scheduled(play_at): each stream whose chunk was timed publishes
//                    (capture time, play_at) as its own clock, the way Playback does for one
//   any thread       playing_capture_us(stream, now), active_streams(now), counters(stream)
//
// Every sender has its own capture clock, so there is one audio clock per stream, never one
// for the mix. The ChunkTime that next_chunk() itself returns is that of the lowest stream id
// with a timed chunk, so Playback's own single clock still works for a two-person call.
//
// Stream ids 0..kStreams-1 (0 outside group calls, 1..proto::kMaxMembers in one). All
// buffers are created up front: the callback never allocates. A stream is "in use" from its
// first packet on; a member who leaves just plays silence until the receiver stops. Someone
// who later joins with the same id restarts that buffer the way a restarted sender would
// (new sequence space -> resync).
//
// Real-time rules as in pa_audio.hpp: next_chunk() and on_scheduled() only copy memory and
// touch atomics.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/pa_audio.hpp"
#include "audio/playout_buffer.hpp"
#include "common/protocol.hpp"

namespace audio {

using MixAccumulator = std::array<int32_t, kChunkSamples>;

// acc += in, sample by sample
inline void mix_add(MixAccumulator& acc, const int16_t* in) {
    for (int i = 0; i < kChunkSamples; ++i) acc[i] += in[i];
}

// out = acc clipped to the int16 range
inline void mix_out(const MixAccumulator& acc, int16_t* out) {
    for (int i = 0; i < kChunkSamples; ++i) {
        const int32_t v = acc[i];
        out[i] = static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}

class StreamMixer : public ChunkSource {
public:
    static constexpr int kStreams = proto::kMaxMembers + 1;  // ids 0..6
    static constexpr int32_t kActiveUs = 2000000;            // "played sound in the last 2 s"

    // prebuffer / max_chunks: for every stream's PlayoutBuffer
    StreamMixer(int prebuffer, int max_chunks);

    static bool valid_stream(uint8_t id) { return id < kStreams; }

    // Network thread only. OutOfRange (and nothing stored) for an invalid stream id.
    PlayoutBuffer::Insert insert(uint8_t stream_id, uint16_t seq, uint32_t capture_us,
                                 const int16_t* samples);

    // Audio callback only.
    using ChunkSource::next_chunk;
    bool next_chunk(int16_t* out, ChunkTime& time) override;
    void on_scheduled(uint32_t play_at_local_us) override;

    // Any thread.
    bool in_use(uint8_t stream_id) const;
    PlayoutBuffer::Counters counters(uint8_t stream_id) const;  // zeros for an invalid id
    int depth_chunks(uint8_t stream_id) const;
    // Sender capture time of stream_id's sound at the speaker at local time now_local_us.
    // false if that stream's last chunk was silence, or it never played.
    bool playing_capture_us(uint8_t stream_id, uint32_t now_local_us, uint32_t& out) const;
    // streams that played timed audio within kActiveUs of now_local_us, ascending
    std::vector<uint8_t> active_streams(uint32_t now_local_us) const;

private:
    std::array<std::unique_ptr<PlayoutBuffer>, kStreams> buffers_;
    std::array<std::atomic<bool>, kStreams> in_use_{};

    // callback only
    MixAccumulator acc_{};
    std::array<int16_t, kChunkSamples> chunk_{};
    std::array<ChunkTime, kStreams> last_time_{};  // each stream's part of the latest chunk

    // published by the callback: (capture_us << 32) | play_at, as in Playback
    std::array<std::atomic<uint64_t>, kStreams> clock_pair_{};
    std::array<std::atomic<bool>, kStreams> clock_valid_{};
    // play_at of each stream's last timed chunk, for active_streams()
    std::array<std::atomic<uint32_t>, kStreams> last_sound_at_{};
    std::array<std::atomic<bool>, kStreams> ever_sound_{};
};

}  // namespace audio
