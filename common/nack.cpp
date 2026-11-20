#include "nack.hpp"

namespace proto {

std::vector<uint8_t> make_nack_packet(uint8_t stream_id, uint16_t nack_seq, const Nack& n) {
    size_t count = n.seqs.size() < kMaxNackSeqs ? n.seqs.size() : kMaxNackSeqs;
    std::vector<uint8_t> out(kHeaderSize + kNackFixedSize + 2 * count);

    Header h{};
    h.type = Type::Nack;
    h.stream_id = stream_id;
    h.seq = nack_seq;
    h.timestamp_us = now_us();
    h.frame_id = 0;
    h.frag_index = 0;
    h.frag_count = 1;
    write_header(h, out.data());

    uint8_t* b = out.data() + kHeaderSize;
    b[0] = static_cast<uint8_t>(n.media);
    b[1] = n.reporter;
    b[2] = static_cast<uint8_t>(count);
    b[3] = 0;
    for (size_t i = 0; i < count; ++i) {
        b[4 + 2 * i] = static_cast<uint8_t>(n.seqs[i] >> 8);
        b[5 + 2 * i] = static_cast<uint8_t>(n.seqs[i]);
    }
    return out;
}

bool read_nack_packet(const uint8_t* data, size_t len, Header& h, Nack& n) {
    if (len < kHeaderSize + kNackFixedSize) return false;
    if (!read_header(data, len, h) || h.type != Type::Nack) return false;

    const uint8_t* b = data + kHeaderSize;
    if (b[0] != static_cast<uint8_t>(Type::Video) && b[0] != static_cast<uint8_t>(Type::Audio))
        return false;
    size_t count = b[2];
    if (count == 0 || count > kMaxNackSeqs) return false;
    // longer is fine: a sealed NACK (phase 6) carries a 40-byte trailer the relay can't strip
    if (len < kHeaderSize + kNackFixedSize + 2 * count) return false;

    n.media = static_cast<Type>(b[0]);
    n.reporter = b[1];
    n.seqs.clear();
    for (size_t i = 0; i < count; ++i)
        n.seqs.push_back(static_cast<uint16_t>((b[4 + 2 * i] << 8) | b[5 + 2 * i]));
    return true;
}

}  // namespace proto
