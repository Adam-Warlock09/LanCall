#pragma once
#include <cstdint>
#include <vector>

#include "protocol.hpp"

namespace net {

using Datagram = std::vector<uint8_t>;

// Splits encoded frames into UDP-sized datagrams (header + up to kMaxPayload bytes).
// One Packetizer per outgoing stream, since it owns that stream's seq and frame counters.
class Packetizer {
public:
    Packetizer(proto::Type type, uint8_t stream_id) : type_(type), stream_id_(stream_id) {}

    // returns the datagrams in order. empty if the frame is too big to describe
    // with an 8-bit piece count (255 * 1400 bytes, about 350 KB)
    std::vector<Datagram> packetize(const uint8_t* data, size_t len, uint32_t timestamp_us);

    uint16_t last_frame_id() const { return static_cast<uint16_t>(frame_id_ - 1); }

private:
    proto::Type type_;
    uint8_t stream_id_;
    uint16_t seq_ = 0;
    uint16_t frame_id_ = 0;
};

}  // namespace net
