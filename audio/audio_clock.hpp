#pragma once
// The audio clock used for lip sync (Person 2, audio/).
//
// Playback publishes, for the newest timed chunk handed to the speaker, its sender capture
// time and the local time its first sample reaches the speaker. Because playback is a
// continuous stream, the sound at the speaker at any local time can be worked out from that
// one pair: before the chunk starts, earlier chunks are still sounding (extrapolate
// backwards); during the chunk, move forward with it.
//
// All times are 32-bit microseconds, compared mod 2^32.

#include <cstdint>

#include "audio/pa_audio.hpp"

namespace audio {

// If the newest chunk started this long ago, the callback has stopped running: no clock.
constexpr int32_t kAudioClockStaleUs = 100000;

// capture_us:    sender capture time of the newest chunk handed to the speaker
// play_local_us: local time that chunk's first sample reaches the speaker
// now_local_us:  local time now
// Sets `out` to the sender capture time of the sound at the speaker now. Past the end of the
// chunk the clock holds at the chunk's end (the next callback is due); after
// kAudioClockStaleUs it reports false.
inline bool audio_clock_at(uint32_t capture_us, uint32_t play_local_us, uint32_t now_local_us,
                           uint32_t& out) {
    int32_t since = static_cast<int32_t>(now_local_us - play_local_us);
    if (since > kAudioClockStaleUs) return false;
    if (since > static_cast<int32_t>(kChunkUs)) since = static_cast<int32_t>(kChunkUs);
    out = capture_us + static_cast<uint32_t>(since);  // a negative `since` wraps correctly
    return true;
}

}  // namespace audio
