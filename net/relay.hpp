#pragma once
// Group-call relay (Person 1, net/), phase 4.
//
// FROZEN after the phase 5 kickoff: change only by agreement.
//
// In relay mode every member sends its audio and video once, to relay_ip:proto::kRelayPort,
// and the relay forwards each packet to every other member. A sender's upload then stays at
// one copy however big the call gets (mesh needs N-1 copies), at the price of one more hop.
// In WebRTC terms it's an SFU (selective forwarding unit): it reads headers, never decodes.
//
// What it does with each datagram (anything without a valid proto::Header is dropped):
//
//   Video, Audio or VideoFec (phase 5 parity, goes where video goes), stream_id s, from ip:port
//       remember member s = ip, and s's video (or audio) socket = ip:port
//       send a copy to every other member at their ip:video_port (or audio_port)
//   Report about stream s (header stream_id), written by member r (the reporter byte)
//       remember member r = ip
//       send it on to s's video or audio socket (the report's media type), so it reaches
//       s's sender exactly as if the receiver had sent it straight there
//   Keepalive, stream_id s
//       remember member s = ip (someone with no camera and no mic still gets media)
//   Nack about stream s from member r (phase 5)
//       resend what's still in the relay's copy of s's last 512 video packets, to r only;
//       if any of it isn't there, pass the NACK on to s's video socket unchanged. s resends
//       all of it: what's in the copy is dropped here as a duplicate, the rest comes in like
//       any video packet and goes to everyone (nobody had it)
//   Phase 6: datagrams are sealed (common/media_crypto.hpp). The relay never has the key
//       and doesn't need it: headers, reports and NACKs stay readable, and it forwards the
//       sealed bytes as they are
//
// Phase 10: a member is (sender ip, stream id). The relay has no key and can't verify
// anything, so it never lets one address take over another's entry: "member s" above means
// every entry with stream id s, each at its own ip. Stream ids outside 1..proto::kMaxMembers
// are ignored, and at most 32 entries exist at once. Two calls sharing one relay both work;
// each also receives the other's packets, which its sockets reject (another key).
//
// Members not heard from for kForgetMs are forgotten. Everything leaves through the relay's
// one socket, so receivers see every stream coming from relay_ip:kRelayPort and send their
// reports back there, with no extra setup.
//
// It needs no signaling: it learns the room from the traffic. It can run inside lancall on
// any member's laptop, or on its own with lancall_relay (e.g. on a laptop that isn't in the
// call at all).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/protocol.hpp"

namespace net {

struct RelayOptions {
    uint16_t port = proto::kRelayPort;        // where members send to
    uint16_t video_port = proto::kVideoPort;  // where members receive video
    uint16_t audio_port = proto::kAudioPort;  // and audio
};

struct RelayMember {
    uint8_t id = 0;  // stream_id = member id
    std::string ip;
    uint64_t packets_in = 0;   // from this member
    uint64_t packets_out = 0;  // forwarded to this member
};

class Relay {
public:
    static constexpr int kForgetMs = 5000;

    Relay();
    ~Relay();  // stops if still running
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    // binds opt.port and starts forwarding on its own thread. false + error() if it can't
    bool start(const RelayOptions& opt = {});
    void stop();
    bool running() const;

    std::vector<RelayMember> members() const;  // in id order, then ip. any thread

    // one line for the UI, e.g. "relay 3 members, in 9.1 Mbps, out 18.2 Mbps"
    std::string stats() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace net
