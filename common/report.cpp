#include "report.hpp"

namespace proto {

namespace {

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

std::vector<uint8_t> make_report_packet(uint8_t stream_id, uint16_t report_seq,
                                        const ReceiverReport& r) {
    std::vector<uint8_t> out(kHeaderSize + kReportBodySize);

    Header h{};
    h.type = Type::Report;
    h.stream_id = stream_id;
    h.seq = report_seq;
    h.timestamp_us = now_us();
    h.frame_id = 0;
    h.frag_index = 0;
    h.frag_count = 1;
    write_header(h, out.data());

    uint8_t* b = out.data() + kHeaderSize;
    b[0] = static_cast<uint8_t>(r.media);
    b[1] = 0;  // reserved
    put16(b + 2, r.highest_seq);
    put16(b + 4, r.loss_permille > 1000 ? 1000 : r.loss_permille);
    put16(b + 6, r.interval_ms);
    put32(b + 8, r.jitter_us);
    put32(b + 12, r.received_kbps);
    return out;
}

bool read_report_packet(const uint8_t* data, size_t len, Header& h, ReceiverReport& r) {
    if (len < kHeaderSize + kReportBodySize) return false;
    if (!read_header(data, len, h) || h.type != Type::Report) return false;

    const uint8_t* b = data + kHeaderSize;
    if (b[0] != static_cast<uint8_t>(Type::Video) && b[0] != static_cast<uint8_t>(Type::Audio))
        return false;
    r.media = static_cast<Type>(b[0]);
    r.highest_seq = get16(b + 2);
    r.loss_permille = get16(b + 4);
    r.interval_ms = get16(b + 6);
    r.jitter_us = get32(b + 8);
    r.received_kbps = get32(b + 12);
    return r.loss_permille <= 1000;
}

}  // namespace proto