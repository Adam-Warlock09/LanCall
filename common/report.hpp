#pragma once
// Receiver reports (phase 3): the receiver of a stream tells its sender how the stream is
// arriving, so the sender can adapt. Loosely modelled on RTCP receiver reports (RFC 3550).
//
// FROZEN after the phase 3 kickoff: change only by agreement.
//
// A report is one normal datagram: the usual 12-byte proto::Header with
//   type = Report, stream_id = the stream being reported on, seq = report counter,
//   timestamp_us = receiver's now_us(), frame_id = 0, frag 0 of 1
// followed by a 16-byte body (big-endian):
//
//   [media type][reserved][highest seq: 2][loss permille: 2][interval ms: 2]
//   [jitter us: 4][received kbps: 4]
//
// Receivers send one every kReportEveryMs to the address their media came FROM (the sender's
// own socket, from_ip:from_port), so it needs no extra port and no configuration.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocol.hpp"

namespace proto {

constexpr size_t kReportBodySize = 16;
constexpr int kReportEveryMs = 500;

struct ReceiverReport {
    Type media = Type::Video;    // which kind of stream this is about
    uint16_t highest_seq = 0;    // highest sequence number received so far
    uint16_t loss_permille = 0;  // packets lost during the interval, per 1000 expected
    uint16_t interval_ms = 0;    // how long the interval was
    uint32_t jitter_us = 0;      // RFC 3550 interarrival jitter estimate
    uint32_t received_kbps = 0;  // receive rate during the interval, headers included
};

// a whole datagram, header included, ready for UdpSocket::send_to
std::vector<uint8_t> make_report_packet(uint8_t stream_id, uint16_t report_seq,
                                        const ReceiverReport& r);

// false if this isn't a well-formed report packet
bool read_report_packet(const uint8_t* data, size_t len, Header& h, ReceiverReport& r);

}  // namespace proto