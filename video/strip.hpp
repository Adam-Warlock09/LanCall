#pragma once
#include <cstddef>
#include <cstdint>

// Each frame is cut into horizontal strips and every strip is its own little JPEG.
// A lost packet then costs one strip instead of the whole frame; the receiver just
// keeps showing the old pixels for that strip until a new one arrives.
//
// Every video payload (what the packetizer splits) starts with this 6-byte prefix,
// followed by the strip's JPEG bytes:
//
//   [index][count][y0 hi][y0 lo][height hi][height lo]
//
// --strips 1 on the sender gives the old whole-frame behaviour with the same format.
namespace video {

constexpr size_t kStripPrefix = 6;

struct StripInfo {
    uint8_t index;          // which strip, 0 = top
    uint8_t count;          // strips per frame
    uint16_t y0;            // first row of this strip in the full frame
    uint16_t frame_height;  // full frame height, so the receiver can size its canvas
};

// big-endian by hand, same byte order as the main header
inline void write_strip(const StripInfo& s, uint8_t* out) {
    out[0] = s.index;
    out[1] = s.count;
    out[2] = static_cast<uint8_t>(s.y0 >> 8);
    out[3] = static_cast<uint8_t>(s.y0 & 0xFF);
    out[4] = static_cast<uint8_t>(s.frame_height >> 8);
    out[5] = static_cast<uint8_t>(s.frame_height & 0xFF);
}

inline bool read_strip(const uint8_t* in, size_t len, StripInfo& s) {
    if (len < kStripPrefix) return false;
    s.index = in[0];
    s.count = in[1];
    s.y0 = static_cast<uint16_t>((in[2] << 8) | in[3]);
    s.frame_height = static_cast<uint16_t>((in[4] << 8) | in[5]);
    return s.count > 0 && s.index < s.count && s.y0 < s.frame_height;
}

}  // namespace video