#pragma once
// NACKs (phase 5): a receiver asks the sender to send some packets again. Loosely modelled
// on RTCP generic NACK (RFC 4585), but as a plain list of sequence numbers.
//
// FROZEN after the phase 5 kickoff: change only by agreement.
//
// One datagram: the usual 12-byte proto::Header with
//   type = Nack, stream_id = the stream the packets belong to, seq = NACK counter,
//   timestamp_us = receiver's now_us(), frame_id = 0, frag 0 of 1
// followed by (big-endian):
//
//   [media type][reporter][count][reserved] [seq: 2] x count
//
// reporter = member id of the receiver asking (like in receiver reports), so the relay
// knows whom to answer. At most kMaxNackSeqs sequence numbers per NACK.
//
// Like reports, NACKs go to the address the media came from: the sender's own socket, or
// the relay, which answers from its own copy when it has one (net/relay.hpp).
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocol.hpp"

namespace proto {

constexpr size_t kNackFixedSize = 4;
constexpr size_t kMaxNackSeqs = 64;

struct Nack {
    Type media = Type::Video;  // which kind of stream the sequence numbers are from
    uint8_t reporter = 0;      // member id of whoever asks, 0 = unknown
    std::vector<uint16_t> seqs;
};

// a whole datagram, header included. seqs beyond kMaxNackSeqs are left out
std::vector<uint8_t> make_nack_packet(uint8_t stream_id, uint16_t nack_seq, const Nack& n);

// false if this isn't a well-formed NACK
bool read_nack_packet(const uint8_t* data, size_t len, Header& h, Nack& n);

}  // namespace proto
