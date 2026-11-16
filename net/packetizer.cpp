#include "packetizer.hpp"

#include <cstring>

namespace net {

std::vector<Datagram> Packetizer::packetize(const uint8_t* data, size_t len,
                                            uint32_t timestamp_us) {
    std::vector<Datagram> out;

    size_t count = (len + proto::kMaxPayload - 1) / proto::kMaxPayload;
    if (count == 0) count = 1;  // still send an empty frame so the receiver sees the id
    if (count > 255) return out;

    // spread bytes evenly instead of 1400,1400,...,tiny. same number of packets either way,
    // but no near-empty last packet wasting a header
    size_t base = len / count;
    size_t extra = len % count;

    proto::Header h{};
    h.type = type_;
    h.stream_id = stream_id_;
    h.timestamp_us = timestamp_us;
    h.frame_id = frame_id_++;
    h.frag_count = static_cast<uint8_t>(count);

    size_t offset = 0;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        size_t piece = base + (i < extra ? 1 : 0);

        h.seq = seq_++;
        h.frag_index = static_cast<uint8_t>(i);

        Datagram d(proto::kHeaderSize + piece);
        proto::write_header(h, d.data());
        if (piece > 0) std::memcpy(d.data() + proto::kHeaderSize, data + offset, piece);
        offset += piece;

        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace net
