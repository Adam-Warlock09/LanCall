#pragma once
// Peer discovery on the floor network (Person 1, control/).
//
// FROZEN after the phase 3 kickoff: change only by agreement.
//
// Every 2 s each laptop broadcasts one text line to 255.255.255.255:proto::kDiscoveryPort
//
//   LANCALL 1 id=3fa9c2d1 name=Aric sig=5011
//
//   1     format version
//   id    random per run, so we can ignore our own beacons and tell restarts apart
//   name  letters, digits, '-' and '_' only, at most 32 characters
//   sig   TCP port where this laptop accepts calls (control/signaling)
//
// A peer that hasn't been heard for 6 s drops off the list. Broadcast only reaches the
// same floor (one /23); calling across floors by IP still works through Signaling::call().
//
// Phase 3: with a directory IP, the same beacon is also sent straight to that directory
// (control/directory.hpp), and the peers it reports from other floors join the list with
// remote = true. A peer heard by broadcast too is shown as local.
//
// Phase 7 (Person 2): two optional fields at the end, which older builds ignore:
//
//   LANCALL 1 id=3fa9c2d1 name=Atul sig=5011 mc=137 title=CN%20lecture
//
//   mc     this laptop sends to multicast group 239.255.77.<mc> (proto::lancall_group):
//          it's broadcasting, or hosting a group call in multicast mode. Other laptops
//          pick a different number for their own group (pick_group)
//   title  only with mc: it's a broadcast anyone on the floor can watch. Escaped with
//          wire::escape (spaces survive), at most 48 bytes before escaping
//
// Both describe our floor only: the directory doesn't pass them on, since multicast can't
// cross the floor routers anyway.
// Keep the rate low: a fast sweep of the network once knocked a test laptop's port offline.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace control {

struct Peer {
    std::string name;
    std::string ip;
    uint16_t signal_port = 0;
    uint32_t last_seen_us = 0;  // proto::now_us() when the last beacon arrived
    bool remote = false;        // heard only through the directory (another floor)
    // phase 7, from its beacon (on our floor only; 0 and empty for remote peers)
    uint8_t group = 0;            // it sends to proto::lancall_group(group); 0 = no group
    std::string broadcast_title;  // non-empty: it's broadcasting this, and can be watched
};

class Discovery {
public:
    Discovery();
    ~Discovery();  // stops if still running
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    // starts beaconing and listening. false + error() on a bad name or if the port is busy.
    // directory_ip empty = this floor only
    bool start(const std::string& my_name, uint16_t signal_port,
               const std::string& directory_ip = "");
    void stop();

    // peers heard in the last 6 s, sorted by name. any thread
    std::vector<Peer> peers() const;
    std::string error() const;

    // Phase 7. What our beacon says from now on: we send to proto::lancall_group(group)
    // (1..254; 0 = nothing), and with a title it's a broadcast. The next beacon goes out at
    // once, not up to 2 s later. Works before start() too. any thread
    void announce(uint8_t group, const std::string& title = "");
    void clear_announce();

    // Phase 7. Our address on the floor network: where our own beacon comes back from, so
    // it's the card the beacons (and so peers' view of us) use. Pass it as the interface
    // for joining and sending to groups. Empty until the first beacon came back. any thread
    std::string own_ip() const;

    // Phase 7. A group number for us: starts from a hash of this run's id and skips every
    // number a peer on our floor announces. 0 if all 254 are taken. any thread
    uint8_t pick_group() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// true if the name is allowed in a beacon (see the format above)
bool valid_name(const std::string& name);

}  // namespace control