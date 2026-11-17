#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace proto {

enum class Type : uint8_t { Video = 1, Audio = 2, Report = 3, Keepalive = 4 };

constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxPayload = 1400;  // 12 + 1400 + UDP/IP headers stays under the 1500 MTU
constexpr uint16_t kVideoPort = 5004;
constexpr uint16_t kAudioPort = 5006;
constexpr uint16_t kDiscoveryPort = 5010;  // UDP broadcast beacons (control/discovery)
constexpr uint16_t kSignalPort = 5011;     // TCP call setup + clock sync (control/signaling)
constexpr uint16_t kDirectoryPort = 5012;  // UDP, cross-floor peer directory (control/directory)
constexpr uint16_t kRelayPort = 5014;      // UDP, group-call relay (net/relay), phase 4
constexpr int kAudioRate = 16000;          // Hz, mono, int16
constexpr int kAudioChunk = 320;           // samples per packet = 20 ms = 640 bytes

// Phase 4 group calls: everyone in a call gets a member id 1..kMaxMembers from the host
// (the host is always 1) and uses it as the stream_id of everything they send, so a receiver
// can tell the streams apart even when they all come from the relay's address.
constexpr int kMaxMembers = 6;

// where to send a stream
struct Endpoint {
    std::string ip;
    uint16_t port = 0;
};

struct Header {
    Type type;
    uint8_t stream_id;
    uint16_t seq;           // per stream, wraps at 65535
    uint32_t timestamp_us;  // sender's steady clock in us, same clock for audio and video
    uint16_t frame_id;      // video frame number, 0 for audio
    uint8_t frag_index;     // 0 .. frag_count-1
    uint8_t frag_count;     // 1 for audio
};

// on the wire everything is big-endian
void write_header(const Header& h, uint8_t* out);
// false if the buffer is too short or the header makes no sense
bool read_header(const uint8_t* in, size_t len, Header& h);

// steady clock in microseconds, cut to 32 bits (wraps every ~71 minutes)
uint32_t now_us();

}  // namespace proto