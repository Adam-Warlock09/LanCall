#pragma once
#include <cstddef>
#include <cstdint>

// Each frame is cut into horizontal strips and every strip is its own little JPEG.
// A lost packet then costs one strip instead of the whole frame; the receiver just
// keeps showing the old pixels for that strip until a new one arrives.
//
// Every video payload (what the packetizer splits) starts with this 8-byte prefix,
// followed by the strip's JPEG bytes (big-endian):
//
//   [index][count][y0: 2][height: 2][send delay: 2]
//
// send delay = time from capture to sending this strip, in units of 100 us (up to 6.5 s).
// The header timestamp is the capture time (lip sync and latency need that), but frames wait
// a varying 0-33 ms before the sender picks them up, plus the encoding of earlier strips. The
// receiver subtracts that wait so its jitter is network jitter only. Without it, the quality
// ladder saw ~10-16 ms of fake jitter on a clean network and never stepped back up (NOTES.md
// issue #6). WebRTC's "abs-send-time" header extension exists for the same reason.
//
// --strips 1 on the sender gives the old whole-frame behaviour with the same format.
namespace video {

constexpr size_t kStripPrefix = 8;
// The largest frame a receiver will draw. The wire allows 65535 rows (and a JPEG can claim any
// width): a packet saying so would make the receiver allocate gigabytes. Cameras stop well short.
constexpr int kMaxFrameWidth = 4096;
constexpr int kMaxFrameHeight = 4096;

struct StripInfo {
    uint8_t index;              // which strip, 0 = top
    uint8_t count;              // strips per frame
    uint16_t y0;                // first row of this strip in the full frame
    uint16_t frame_height;      // full frame height, so the receiver can size its canvas
    uint16_t send_delay_100us;  // capture -> send, in 100 us units
};

// capture -> send in 100 us units, capped so it fits in 16 bits
inline uint16_t send_delay_units(uint32_t capture_us, uint32_t now_us) {
    uint32_t d = (now_us - capture_us) / 100;
    return static_cast<uint16_t>(d > 0xFFFF ? 0xFFFF : d);
}

inline void write_strip(const StripInfo& s, uint8_t* out) {
    out[0] = s.index;
    out[1] = s.count;
    out[2] = static_cast<uint8_t>(s.y0 >> 8);
    out[3] = static_cast<uint8_t>(s.y0 & 0xFF);
    out[4] = static_cast<uint8_t>(s.frame_height >> 8);
    out[5] = static_cast<uint8_t>(s.frame_height & 0xFF);
    out[6] = static_cast<uint8_t>(s.send_delay_100us >> 8);
    out[7] = static_cast<uint8_t>(s.send_delay_100us & 0xFF);
}

// false for anything a receiver can't use: no JPEG after the prefix (decoding an empty buffer
// throws in OpenCV), a frame taller than kMaxFrameHeight, a strip outside its frame
inline bool read_strip(const uint8_t* in, size_t len, StripInfo& s) {
    if (len <= kStripPrefix) return false;
    s.index = in[0];
    s.count = in[1];
    s.y0 = static_cast<uint16_t>((in[2] << 8) | in[3]);
    s.frame_height = static_cast<uint16_t>((in[4] << 8) | in[5]);
    s.send_delay_100us = static_cast<uint16_t>((in[6] << 8) | in[7]);
    return s.count > 0 && s.index < s.count && s.y0 < s.frame_height &&
           s.frame_height <= kMaxFrameHeight;
}

}  // namespace video
