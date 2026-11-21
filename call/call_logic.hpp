#pragma once
// Pure helpers of the lancall app for the control plane (Person 2, call/): who `call <arg>`
// means, the status line for each call state, and the `list` output. No I/O, so they are
// unit-tested on their own (tests/metrics/test_call_logic.cpp).

#include <cstdint>
#include <string>
#include <vector>

#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/signaling.hpp"

namespace call {

struct Target {
    std::string name;  // peer name if known (may be empty when called by IP)
    std::string ip;
    uint16_t port = proto::kSignalPort;
    std::string error;  // why the target could not be resolved, ready to print
    bool ok() const { return error.empty(); }
};

// `call <arg>`: arg is an IPv4 address (works across floors, where discovery does not reach)
// or a peer name from `list`. Names match exactly first, then ignoring case if that is
// unambiguous. Two peers with the same name must be called by IP.
Target resolve_target(const std::string& arg, const std::vector<control::Peer>& peers);

// Phase 6, `text <arg> <message>`: someone in our call first (by name, exactly then ignoring
// case, or by IP), since members on other floors may not be in `list`; otherwise the same
// as resolve_target(). members is Signaling::members() (empty outside a call).
Target resolve_text_target(const std::string& arg, const std::vector<control::Member>& members,
                           const std::vector<control::Peer>& peers);

// What the window's status line says.
struct StatusInfo {
    std::string my_name;
    control::CallState state = control::CallState::Idle;
    std::string peer_name, peer_ip;
    std::string media_parts;      // e.g. "audio+video"; used while InCall
    double rtt_ms = 0;            // 0 = unknown
    int people = 0;               // phase 4: everyone in the call, us included (0 = unknown)
    std::string relay_ip;         // phase 4: media goes through this relay; empty = mesh
    std::string multicast_group;  // phase 7: media goes to this group; empty = not multicast
};
std::string status_line(const StatusInfo& s);

// The `list` output, one line per peer, given the current proto::now_us(). Since phase 7 a
// peer that's broadcasting gets `broadcasting "title" (watch <name>)` on its line.
std::string format_peers(const std::vector<control::Peer>& peers, uint32_t now_us);

// ---- phase 7: broadcasts ----------------------------------------------------------------

// Whose broadcast `watch <arg>` means.
struct BroadcastTarget {
    std::string name, ip, title;
    uint8_t group = 0;  // proto::lancall_group(group) is what to join
    std::string error;  // why there's nothing to watch, ready to print
    bool ok() const { return error.empty(); }
};

// arg is a name (exactly, then ignoring case) or an IP. Of the peers it matches, the one
// that's broadcasting on our floor; peers on other floors can't be watched (multicast doesn't
// leave its floor). Two copies on one laptop share an IP, hence "of the peers it matches".
BroadcastTarget resolve_broadcast(const std::string& arg, const std::vector<control::Peer>& peers);

// The window's status line while broadcasting or watching.
std::string broadcast_status(const std::string& my_name, const std::string& title,
                             const std::string& group_ip);
std::string watch_status(const std::string& my_name, const std::string& presenter,
                         const std::string& title);

// ---- phase 4: group calls -------------------------------------------------------------

// Where our media goes and whose media we accept. Rebuilt after every change of the room
// (someone joined or left, mesh <-> relay) and handed to MediaSession::update_route().
struct Route {
    std::vector<proto::Endpoint> audio_to;  // Signaling::media_destinations(kAudioPort)
    std::vector<proto::Endpoint> video_to;  // Signaling::media_destinations(kVideoPort)
    std::vector<std::string> sources;       // media_sources() below; empty = anyone
};

// Direct mode (--peer, no signaling): everything to and from that one address.
Route direct_route(const std::string& peer_ip);

// The addresses media may come from: in relay mode only the relay (it forwards everyone's
// streams from its own address), in mesh mode every other member, and in multicast mode
// (phase 7) every other member too: a packet sent to a group arrives from its sender's own
// address. Members without a known ip are left out.
std::vector<std::string> media_sources(const std::vector<control::Member>& members,
                                       control::MediaMode mode, const std::string& relay_ip);

// The `members` output: one line per member in id order, then how media flows. address is
// the relay's ip in relay mode and the group in multicast mode (unused in mesh mode).
std::string format_members(const std::vector<control::Member>& members, control::MediaMode mode,
                           const std::string& address);

// Phase 7: the members (us left out) that multicast can't reach: discovery doesn't hear them
// on our floor, whether they're on another floor or not heard at all. Multicast stops at the
// floor router, so a multicast call needs this to be empty.
std::vector<control::Member> off_floor_members(const std::vector<control::Member>& members,
                                               const std::vector<control::Peer>& peers);

// The name to show for a member: their name, else their ip, else "member <id>".
std::string member_label(const control::Member& m);

// Whether a call's received stream belongs on screen: stream 0 (a peer without a member id),
// anything while the member list is still empty (the start of a call), else only the ids of
// members other than us. Frames still queued from someone who just left are dropped by this.
bool accepts_stream(uint8_t stream_id, const std::vector<control::Member>& members);

// The one other member when exactly two are in the call, else nullptr.
const control::Member* other_member(const std::vector<control::Member>& members);

// In relay mode the relay learns who is in the call only from what they send, and forgets
// anyone silent for net::Relay::kForgetMs (5 s). A member whose mic and camera both failed
// would never be heard, and would get nothing. So in relay mode the app sends this 12-byte
// "still here" packet to relay_ip:proto::kRelayPort every kKeepaliveEveryUs, always (it
// costs ~6 bytes/s): a bare header, type Keepalive, stream_id = our member id.
constexpr uint32_t kKeepaliveEveryUs = 2000000;
std::vector<uint8_t> keepalive_packet(uint8_t my_id, uint16_t seq, uint32_t now_us);

}  // namespace call
