#include "audio/mixer.hpp"

#include "audio/audio_clock.hpp"

namespace audio {

StreamMixer::StreamMixer(int prebuffer, int max_chunks) {
    for (auto& b : buffers_) b = std::make_unique<PlayoutBuffer>(prebuffer, max_chunks);
}

PlayoutBuffer::Insert StreamMixer::insert(uint8_t stream_id, uint16_t seq, uint32_t capture_us,
                                          const int16_t* samples) {
    if (!valid_stream(stream_id)) return PlayoutBuffer::Insert::OutOfRange;
    const PlayoutBuffer::Insert r = buffers_[stream_id]->insert(seq, capture_us, samples);
    // after the insert, so the callback never pulls from a buffer that has seen no packet
    in_use_[stream_id].store(true, std::memory_order_release);
    return r;
}

bool StreamMixer::next_chunk(int16_t* out, ChunkTime& time) {
    time = ChunkTime{};
    acc_.fill(0);
    for (int i = 0; i < kStreams; ++i) {
        last_time_[i] = ChunkTime{};
        if (!in_use_[i].load(std::memory_order_acquire)) continue;
        buffers_[i]->next_chunk(chunk_.data(), last_time_[i]);
        mix_add(acc_, chunk_.data());
        if (last_time_[i].valid && !time.valid) time = last_time_[i];  // lowest id wins
    }
    mix_out(acc_, out);
    return true;
}

void StreamMixer::on_scheduled(uint32_t play_at_local_us) {
    for (int i = 0; i < kStreams; ++i) {
        if (!last_time_[i].valid) {
            clock_valid_[i].store(false, std::memory_order_release);
            continue;
        }
        clock_pair_[i].store(
            (static_cast<uint64_t>(last_time_[i].capture_us) << 32) | play_at_local_us,
            std::memory_order_release);
        clock_valid_[i].store(true, std::memory_order_release);
        last_sound_at_[i].store(play_at_local_us, std::memory_order_relaxed);
        ever_sound_[i].store(true, std::memory_order_release);
    }
}

bool StreamMixer::in_use(uint8_t stream_id) const {
    return valid_stream(stream_id) && in_use_[stream_id].load(std::memory_order_acquire);
}

PlayoutBuffer::Counters StreamMixer::counters(uint8_t stream_id) const {
    return valid_stream(stream_id) ? buffers_[stream_id]->counters() : PlayoutBuffer::Counters{};
}

int StreamMixer::depth_chunks(uint8_t stream_id) const {
    return valid_stream(stream_id) ? buffers_[stream_id]->depth_chunks() : 0;
}

bool StreamMixer::playing_capture_us(uint8_t stream_id, uint32_t now_local_us,
                                     uint32_t& out) const {
    if (!valid_stream(stream_id) || !clock_valid_[stream_id].load(std::memory_order_acquire))
        return false;
    const uint64_t pair = clock_pair_[stream_id].load(std::memory_order_acquire);
    return audio_clock_at(static_cast<uint32_t>(pair >> 32), static_cast<uint32_t>(pair),
                          now_local_us, out);
}

std::vector<uint8_t> StreamMixer::active_streams(uint32_t now_local_us) const {
    std::vector<uint8_t> out;
    for (int i = 0; i < kStreams; ++i) {
        if (!ever_sound_[i].load(std::memory_order_acquire)) continue;
        // play_at is a little in the future (output latency): negative = playing right now
        const int32_t since =
            static_cast<int32_t>(now_local_us - last_sound_at_[i].load(std::memory_order_relaxed));
        if (since < kActiveUs) out.push_back(static_cast<uint8_t>(i));
    }
    return out;
}

}  // namespace audio
