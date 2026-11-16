#pragma once
// Latency and lip-sync arithmetic for experiment E1 (Person 2, metrics/).
//
// Every timestamp is proto::now_us(): microseconds since that laptop booted, cut to 32 bits.
// Two laptops' clocks can therefore be hours apart, far outside a signed 32-bit range, and
// each wraps every ~71.6 minutes. All arithmetic here is unsigned (mod 2^32); only the final,
// small delay is cast to int32_t, so results are correct as long as the true delay is within
// about +-35 minutes.

#include <cstdint>

namespace metrics {

// One-way delay of media captured at `capture_us` on the PEER's clock and observed (shown or
// played) at `local_now_us` on OUR clock.
// `offset_us` follows Signaling::clock_offset_us(): peer_clock ~= local_clock + offset (mod 2^32).
// Use offset 0 when sender and receiver run on the same laptop.
inline int32_t e2e_us(uint32_t local_now_us, uint32_t offset_us, uint32_t capture_us) {
    return static_cast<int32_t>(local_now_us + offset_us - capture_us);
}

// Lip-sync offset between the video frame being shown and the audio being heard, both stamped
// by the same sender's clock: positive = video is ahead of the audio. No clock offset needed.
inline int32_t av_offset_us(uint32_t video_capture_us, uint32_t audio_capture_us) {
    return static_cast<int32_t>(video_capture_us - audio_capture_us);
}

// True if time `a` is later than time `b` (mod 2^32).
inline bool time_after(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

inline double us_to_ms(int32_t us) {
    return us / 1000.0;
}

}  // namespace metrics
