#pragma once
// Peer discovery on the floor network (Person 1, control/).
//
// FROZEN after the phase 2 kickoff: change only by agreement.
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
};

class Discovery {
public:
    Discovery();
    ~Discovery();  // stops if still running
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    // starts beaconing and listening. false + error() on a bad name or if the port is busy
    bool start(const std::string& my_name, uint16_t signal_port);
    void stop();

    // peers heard in the last 6 s, sorted by name. any thread
    std::vector<Peer> peers() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// true if the name is allowed in a beacon (see the format above)
bool valid_name(const std::string& name);

}  // namespace control