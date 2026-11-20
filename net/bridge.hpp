#pragma once
// Broadcast bridge to another floor (Person 2, net/, phase 7).
//
// A broadcast is multicast, and multicast never crosses the floor routers, so on its own a
// broadcast reaches only the presenter's floor. A bridge runs on a laptop on another floor:
// the presenter sends it one extra unicast copy of the broadcast (`bridge <ip>` in lancall),
// to bridge_ip:kBridgePort, and the bridge sends every datagram on, unchanged, to a multicast
// group on its own floor. Viewers there watch it like any other broadcast (lancall_bridge
// announces it in the discovery beacons).
//
//   floor A: presenter --multicast--> group (A's viewers)
//                 `----- one unicast copy -----> bridge on floor B --multicast--> group (B)
//
// What it does with each datagram arriving on its port:
//   Video, VideoFec, Audio from the presenter (from_ip only)
//       send it to group:video_port (Video, VideoFec) or group:audio_port (Audio), as is.
//       Anything else claiming to be media is dropped: nobody else can feed the floor's group
//   Report (a viewer on this floor, reporting on what the bridge sends it)
//       back to the presenter's socket for that media, so its viewer count includes them
//   anything else (NACKs: the presenter doesn't resend for broadcasts) is dropped
//
// Broadcasts are not encrypted (phase 7 v1), so there's nothing to open or seal here; the
// bridge forwards the bytes exactly as they came. One socket, one thread, no decoding.
#include <cstdint>
#include <memory>
#include <string>

#include "common/protocol.hpp"

namespace net {

constexpr uint16_t kBridgePort = 5016;  // UDP, where a bridge takes the presenter's copy

struct BridgeOptions {
    uint16_t port = kBridgePort;
    std::string from_ip;   // the presenter: only its media is passed on (required)
    std::string group;     // the group on this floor, e.g. proto::lancall_group(n)
    std::string iface_ip;  // our wired address, the card the group packets leave by; "" = any
    bool loop = true;      // this laptop gets the group too, so it can watch with lancall
    uint16_t video_port = proto::kVideoPort;  // the viewers' ports
    uint16_t audio_port = proto::kAudioPort;
    int quiet_ms = 5000;  // live() turns false after this long without the presenter's media
};

struct BridgeCounters {
    uint64_t forwarded = 0;      // media datagrams sent on to the group
    uint64_t reports_back = 0;   // viewers' reports passed back to the presenter
    uint64_t dropped_other = 0;  // media from anyone but the presenter, NACKs, ...
    uint64_t dropped_bad = 0;    // not a LanCall datagram at all
};

class Bridge {
public:
    Bridge();
    ~Bridge();  // stops if still running
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // binds opt.port and starts forwarding on its own thread. false + error() if it can't
    bool start(const BridgeOptions& opt);
    void stop();
    bool running() const;

    // the presenter's media arrived within quiet_ms: the broadcast is on. any thread
    bool live() const;

    BridgeCounters counters() const;  // any thread
    // one line for the UI, refreshed every second, e.g.
    // "bridge 10.8.64.5 -> 239.255.77.9: 4.9 Mbps, 460 pkt/s, 2 reports/s back"
    std::string stats() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace net
