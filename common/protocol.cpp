#include "protocol.hpp"

#include <arpa/inet.h>

#include <chrono>
#include <cstring>

namespace proto {

void write_header(const Header& h, uint8_t* out) {
    uint16_t seq = htons(h.seq);
    uint32_t ts = htonl(h.timestamp_us);
    uint16_t fid = htons(h.frame_id);

    out[0] = static_cast<uint8_t>(h.type);
    out[1] = h.stream_id;
    std::memcpy(out + 2, &seq, 2);  // memcpy because out+2 isn't guaranteed to be aligned
    std::memcpy(out + 4, &ts, 4);
    std::memcpy(out + 8, &fid, 2);
    out[10] = h.frag_index;
    out[11] = h.frag_count;
}

bool read_header(const uint8_t* in, size_t len, Header& h) {
    if (len < kHeaderSize) return false;

    uint8_t t = in[0];
    if (t < 1 || t > 4) return false;

    uint16_t seq, fid;
    uint32_t ts;
    std::memcpy(&seq, in + 2, 2);
    std::memcpy(&ts, in + 4, 4);
    std::memcpy(&fid, in + 8, 2);

    h.type = static_cast<Type>(t);
    h.stream_id = in[1];
    h.seq = ntohs(seq);
    h.timestamp_us = ntohl(ts);
    h.frame_id = ntohs(fid);
    h.frag_index = in[10];
    h.frag_count = in[11];

    // a piece index outside its own count means the packet is garbage
    if (h.frag_count == 0 || h.frag_index >= h.frag_count) return false;
    return true;
}

uint32_t now_us() {
    using namespace std::chrono;
    auto us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(us);
}

}  // namespace proto
