#pragma once
// Call setup, group calls and clock sync over TCP (Person 1, control/).
//
// FROZEN after the phase 4 kickoff: change only by agreement.
//
// Messages are text lines ("\n" ended), so a call can be poked by hand with `nc <ip> 5011`:
//
//   INVITE name=Aric id=<n>         caller -> callee, right after connecting. id (phase 4) =
//                                   the member id the host has set aside for the callee
//   ACCEPT name=Atul                callee -> caller, call is up
//   DECLINE reason=busy|declined    callee -> caller, then the connection closes
//   BYE                             either side, then the connection closes
//   PING t1=<us>                    clock sync, either side, every 2 s during a call
//   PONG t1=<us> t2=<us> t3=<us>    t2 = when PING arrived, t3 = when PONG was sent
//
// Phase 4, group calls. Whoever starts a call is the host and keeps one TCP connection to
// every member; members only ever talk to the host (a star, so N people need N-1
// connections, not N*(N-1)/2). The host gives out member ids, host = 1, in join order, and
// tells everyone who is in the room:
//
//   WELCOME id=<n>                          host -> new member, right after ACCEPT
//   MEMBER id=<n> name=<s> ip=<a> off=<us>  host -> members: n is in the room. sent for
//                                           everyone already there when someone joins, to
//                                           everyone when someone new joins, and again
//                                           every 2 s with a fresh off. ip=- means "me, the
//                                           host": use the address of this connection.
//                                           off = n's clock minus the host's (mod 2^32)
//   LEFT id=<n> reason=<s>                  host -> members: n hung up or dropped
//   MEDIA mode=mesh|relay relay=<ip>        host -> members: where media goes (see below)
//
// Members sync clocks with the host only (PING/PONG as before). Combining that with the
// off= values gives every member's offset to every other member: two LAN round trips of
// error instead of one, still well under a millisecond.
//
// A member who hangs up sends BYE to the host, and the host sends LEFT to the others. When
// the host hangs up, the call ends for everyone (no host hand-over). When everyone else has
// left, the host's call ends too. A two-person call is just a room of two, and the extra
// messages are ignored by older builds, so it still works against a phase 3 lancall.
//
// Media: in mesh mode every member sends its audio and video to every other member (N-1
// copies of each stream). In relay mode everyone sends one copy to the relay
// (relay_ip:proto::kRelayPort, see net/relay.hpp), which forwards it to the others.
// Either way stream_id = member id, so receivers tell the streams apart.
//
// States:   Idle --call()--> Calling --ACCEPT--> InCall --hang_up()/BYE--> Idle
//           Idle --INVITE--> Ringing --accept()--> InCall
//                                    --decline()--> Idle
// An INVITE while not Idle is answered with DECLINE reason=busy. Calling gives up after
// 30 s with no answer (Failed, "no answer"). A dropped connection ends the call (Ended), or
// for the host, just removes that member (Left). So does 10 s without a single line from
// the other side (reason "no response"): both sides PING every 2 s, so that only happens
// when the other laptop vanished without closing the connection (cable pulled, asleep).
//
// The host adding someone mid-call (invite()) stays InCall: the new person rings, and the
// answer arrives as Joined, or as Declined/Failed with that person's name and ip.
//
// Clock sync: offset = ((t2 - t1) + (t3 - t4)) / 2, rtt = (t4 - t1) - (t3 - t2), where
// t4 is when the PONG came back. The sample with the smallest rtt of the last 8 wins, and
// samples with rtt over 100 ms are thrown away (TCP resent them, see NOTES.md issue #7).
// Laptop clocks count from boot, so the offset can be hours: everything is modulo 2^32.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/protocol.hpp"

namespace control {

enum class CallState { Idle, Calling, Ringing, InCall };

enum class MediaMode { Mesh, Relay };

struct Member {
    uint8_t id = 0;  // 1..proto::kMaxMembers, also the stream_id of their media. host = 1
    std::string name;
    std::string ip;
    bool me = false;  // this entry is us
};

struct CallEvent {
    enum class Kind {
        Incoming,      // someone is calling us: accept() or decline()
        Accepted,      // our call was accepted, or we accepted theirs: start media now
        Declined,      // our call was declined (reason says why)
        Ended,         // the call is over (BYE, hang_up() or a dropped connection): stop media
        Failed,        // our call never connected (no answer, refused, unreachable)
        Joined,        // phase 4: someone joined the room we're in (members() has them now)
        Left,          // phase 4: someone left the room; the call goes on for the rest
        MediaChanged,  // phase 4: the host switched mesh/relay: re-point media_destinations()
    };
    Kind kind;
    std::string peer_name;
    std::string peer_ip;
    std::string reason;     // for Declined / Failed / Ended / Left, may be empty
    uint8_t member_id = 0;  // for Joined / Left, and Accepted once ids are known
};

class Signaling {
public:
    Signaling();
    ~Signaling();  // hangs up and stops if still running
    Signaling(const Signaling&) = delete;
    Signaling& operator=(const Signaling&) = delete;

    // listen for incoming calls. false + error() if the port is busy
    bool start(const std::string& my_name, uint16_t port = proto::kSignalPort);
    void stop();

    // All of these return right away; what happens next arrives through poll_event().
    // call() only works from Idle, accept()/decline() while Ringing,
    // hang_up() while Calling or InCall. Anything else is ignored.
    bool call(const std::string& ip, uint16_t port = proto::kSignalPort);
    void accept();
    void decline();
    void hang_up();

    // Phase 4. Host only, while InCall: ring one more person into the room. The answer
    // comes back as Joined, Declined or Failed. false if we're not the host, the room is
    // full (proto::kMaxMembers) or that ip is already in it. From Idle it is the same as
    // call(), and we become the host.
    bool invite(const std::string& ip, uint16_t port = proto::kSignalPort);

    // Phase 4. Host only: how media should flow. Sent to every member now and to anyone who
    // joins later, and they each get a MediaChanged event. Default: mesh. relay_ip may be
    // anyone's laptop running a relay, including the host's own.
    void set_media(MediaMode mode, const std::string& relay_ip = "");
    MediaMode media_mode() const;
    std::string relay_ip() const;  // empty in mesh mode

    CallState state() const;
    // with more than two people: the host for a member, the first member for the host
    std::string peer_name() const;  // empty when Idle
    std::string peer_ip() const;

    // Phase 4. everyone in the call, us included (me = true), in id order. empty unless InCall
    std::vector<Member> members() const;
    uint8_t my_id() const;  // our member id = the stream_id to send with. 0 unless InCall
    bool is_host() const;

    // Phase 4. where one of our streams goes right now: {ip, mesh_port} of every other member
    // in mesh mode, or just {relay_ip, proto::kRelayPort} in relay mode. e.g.
    //   video_tx.set_destinations(sig.media_destinations(proto::kVideoPort));
    std::vector<proto::Endpoint> media_destinations(uint16_t mesh_port) const;

    // next event, or false if there is none. never blocks. for the app's main loop
    bool poll_event(CallEvent& out);

    // peer clock minus our clock, modulo 2^32. false until the first PONG of this call.
    // peer time -> our time:  ours = static_cast<uint32_t>(theirs - offset)
    bool clock_offset(uint32_t& peer_minus_mine_us) const;
    double rtt_ms() const;  // of the sample in use, 0 when unknown

    // Phase 4: the same for one member of a group call (see "Members sync clocks" above).
    // false until both halves of the chain are known. Our own id gives 0.
    bool clock_offset(uint8_t member_id, uint32_t& member_minus_mine_us) const;

    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace control