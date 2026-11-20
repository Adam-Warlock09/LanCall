#pragma once
// Call setup, group calls and clock sync over TCP (Person 1, control/).
//
// FROZEN after the phase 6 kickoff: change only by agreement.
//
// Messages are text lines ("\n" ended). Since phase 6 everything after the first line each
// way is encrypted, so `nc <ip> 5011` only shows the handshake (control_demo still works):
//
//   HELLO v=1 pk=<hex>              both sides, first thing, in the clear. pk = a fresh public
//                                   key per connection; every line after the two HELLOs goes
//                                   as "S <hex>", encrypted (control/secure_line.hpp)
//   INVITE name=Aric id=<n> key=<hex>  caller -> callee. id (phase 4) = the member id the host
//                                   has set aside for the callee, key (phase 6) = the call's
//                                   media key, made by the host
//   ACCEPT name=Atul                callee -> caller, call is up
//   DECLINE reason=busy|declined    callee -> caller, then the connection closes
//   BYE                             either side, then the connection closes
//   PING t1=<us>                    clock sync, either side, every 2 s during a call
//   PONG t1=<us> t2=<us> t3=<us>    t2 = when PING arrived, t3 = when PONG was sent
//
// A lancall that doesn't encrypt (phase 6 chat and older) is turned away: calling one fails
// with "no encrypted handshake (an older LanCall there?)", and being called by one sends it
// DECLINE reason=old-version. A line that doesn't decrypt ends that connection. Message-only
// connections (MSG, below) do the same handshake first, so chat is encrypted too.
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
//   MEDIA mode=mesh|relay|multicast         host -> members: where media goes (see below)
//         [relay=<ip>] [group=<ip>]           relay= in relay mode, group= in multicast mode
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
// Phase 7, multicast mode (everyone on one floor): everyone sends one copy to a multicast
// group on the normal media ports and joins that group to receive. Multicast never leaves
// the floor, so the host should only pick it when every member is on its own floor.
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
// Phase 6, text chat. Text goes over the same TCP connections, escaped by wire::escape()
// (so spaces and '=' survive the key=value format), at most kMaxTextBytes once unescaped:
//
//   CHAT to=all|<id> text=<t>                      member -> host
//   CHAT from=<id> name=<s> to=all|<id> text=<t>   host -> member(s)
//
// In a call the host passes messages on: one to everyone goes to all the others, one to a
// single member goes to that member only (so the host's laptop does see private messages
// between two members). Someone not in our call gets a connection of their own:
//
//   MSG name=<s> text=<t>          sender -> them, as the first line of a new connection
//   OK                             them -> sender, then the connection closes
//
// A new connection's first line decides what it is: INVITE goes on as a call (or gets
// DECLINE reason=busy), MSG is a message, anything else closes it. So messages arrive
// whether or not the receiver is in a call. Older builds answer MSG with nothing and drop
// CHAT, so text to them just doesn't arrive.
//
// Clock sync: offset = ((t2 - t1) + (t3 - t4)) / 2, rtt = (t4 - t1) - (t3 - t2), where
// t4 is when the PONG came back. The sample with the smallest rtt of the last 8 wins, and
// samples with rtt over 100 ms are thrown away (TCP resent them, see NOTES.md issue #7).
// Laptop clocks count from boot, so the offset can be hours: everything is modulo 2^32.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/protocol.hpp"

namespace control {

constexpr size_t kMediaKeyBytes = 32;  // = proto::MediaCrypto::kKeyBytes

enum class CallState { Idle, Calling, Ringing, InCall };

enum class MediaMode { Mesh, Relay, Multicast };

struct Member {
    uint8_t id = 0;  // 1..proto::kMaxMembers, also the stream_id of their media. host = 1
    std::string name;
    std::string ip;
    bool me = false;  // this entry is us
};

// Phase 6: a text message for us, or one of ours that couldn't be delivered
struct ChatEvent {
    enum class Kind {
        Received,  // name/ip/member_id say who from, to_all whether everyone got it
        Failed,    // ours to ip couldn't be delivered (reason says why); text is what it was
    };
    Kind kind = Kind::Received;
    std::string name;       // sender ("unknown" if their name wasn't valid)
    std::string ip;         // sender for Received, where it was going for Failed
    uint8_t member_id = 0;  // the sender's id when it came through our call, else 0
    bool to_all = false;    // sent to everyone in the call (false: just to us)
    std::string text;       // control characters are replaced with spaces
    std::string reason;     // for Failed
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
    // joins later, and they each get a MediaChanged event. Default: mesh. `address` is the
    // relay's IP in relay mode (anyone's laptop, the host's own included) and the group in
    // multicast mode (phase 7, e.g. proto::lancall_group(...)); a bad one is ignored.
    void set_media(MediaMode mode, const std::string& address = "");
    MediaMode media_mode() const;
    std::string relay_ip() const;         // empty unless relay mode
    std::string multicast_group() const;  // phase 7: empty unless multicast mode

    CallState state() const;
    // with more than two people: the host for a member, the first member for the host
    std::string peer_name() const;  // empty when Idle
    std::string peer_ip() const;

    // Phase 4. everyone in the call, us included (me = true), in id order. empty unless InCall
    std::vector<Member> members() const;
    uint8_t my_id() const;  // our member id = the stream_id to send with. 0 unless InCall
    bool is_host() const;

    // Phase 4. where one of our streams goes right now: {ip, mesh_port} of every other member
    // in mesh mode, just {relay_ip, proto::kRelayPort} in relay mode, or {group, mesh_port}
    // in multicast mode (phase 7). e.g.
    //   video_tx.set_destinations(sig.media_destinations(proto::kVideoPort));
    std::vector<proto::Endpoint> media_destinations(uint16_t mesh_port) const;

    // next event, or false if there is none. never blocks. for the app's main loop
    bool poll_event(CallEvent& out);

    // Phase 6, text chat (wire format above). Both return right away; false if the text is
    // empty or longer than kMaxTextBytes, or for text_all() when we're not InCall.
    // text_all: to everyone else in our call.
    // text_to: to one person. If ip is a member of our call it goes through the call,
    // otherwise over a short connection of its own to ip:port (anyone, in a call or not).
    // Messages for us, and ours that failed, come out of poll_chat().
    static constexpr size_t kMaxTextBytes = 500;
    bool text_all(const std::string& text);
    bool text_to(const std::string& ip, const std::string& text,
                 uint16_t port = proto::kSignalPort);
    bool poll_chat(ChatEvent& out);

    // peer clock minus our clock, modulo 2^32. false until the first PONG of this call.
    // peer time -> our time:  ours = static_cast<uint32_t>(theirs - offset)
    bool clock_offset(uint32_t& peer_minus_mine_us) const;
    double rtt_ms() const;  // of the sample in use, 0 when unknown

    // Phase 4: the same for one member of a group call (see "Members sync clocks" above).
    // false until both halves of the chain are known. Our own id gives 0.
    bool clock_offset(uint8_t member_id, uint32_t& member_minus_mine_us) const;

    // Phase 6: the call's media key, the same for everyone in the call. Give it to the media
    // with proto::MediaCrypto (common/media_crypto.hpp). false unless InCall
    bool media_key(std::array<uint8_t, kMediaKeyBytes>& out) const;

    // Phase 6: "1234 5678", the code to compare with that member to rule out someone in the
    // middle. Only the host and a member share a connection, so a member gets one for the
    // host (id 1) and the host one for every member; "" otherwise
    std::string safety_code(uint8_t member_id) const;

    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace control