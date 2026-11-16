#pragma once
// Call setup and clock sync over TCP (Person 1, control/).
//
// FROZEN after the phase 2 kickoff: change only by agreement.
//
// One TCP connection per call, on proto::kSignalPort. Messages are text lines ("\n" ended),
// so a call can be poked by hand with `nc <ip> 5011`:
//
//   INVITE name=Aric                caller -> callee, right after connecting
//   ACCEPT name=Atul                callee -> caller, call is up
//   DECLINE reason=busy|declined    callee -> caller, then the connection closes
//   BYE                             either side, then the connection closes
//   PING t1=<us>                    clock sync, either side, every 2 s during a call
//   PONG t1=<us> t2=<us> t3=<us>    t2 = when PING arrived, t3 = when PONG was sent
//
// All times are proto::now_us() on the side that took them. Unknown messages are ignored.
//
// States:   Idle --call()--> Calling --ACCEPT--> InCall --hang_up()/BYE--> Idle
//           Idle --INVITE--> Ringing --accept()--> InCall
//                                    --decline()--> Idle
// An INVITE while not Idle is answered with DECLINE reason=busy. Calling gives up after
// 30 s with no answer (Failed, "no answer"). A dropped connection ends the call (Ended).
//
// Clock sync: offset = ((t2 - t1) + (t3 - t4)) / 2, rtt = (t4 - t1) - (t3 - t2), where
// t4 is when the PONG came back. The sample with the smallest rtt of the last 8 wins.
// Laptop clocks count from boot, so the offset can be hours: everything is modulo 2^32.
#include <cstdint>
#include <memory>
#include <string>

#include "common/protocol.hpp"

namespace control {

enum class CallState { Idle, Calling, Ringing, InCall };

struct CallEvent {
    enum class Kind {
        Incoming,  // someone is calling us: accept() or decline()
        Accepted,  // our call was accepted, or we accepted theirs: start media now
        Declined,  // our call was declined (reason says why)
        Ended,     // the call is over (BYE, hang_up() or a dropped connection): stop media
        Failed,    // our call never connected (no answer, refused, unreachable)
    };
    Kind kind;
    std::string peer_name;
    std::string peer_ip;
    std::string reason;  // for Declined / Failed / Ended, may be empty
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

    CallState state() const;
    std::string peer_name() const;  // empty when Idle
    std::string peer_ip() const;

    // next event, or false if there is none. never blocks. for the app's main loop
    bool poll_event(CallEvent& out);

    // peer clock minus our clock, modulo 2^32. false until the first PONG of this call.
    // peer time -> our time:  ours = static_cast<uint32_t>(theirs - offset)
    bool clock_offset(uint32_t& peer_minus_mine_us) const;
    double rtt_ms() const;  // of the sample in use, 0 when unknown

    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace control