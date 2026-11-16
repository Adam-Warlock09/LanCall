#pragma once
// Peer directory for calls across floors (Person 1, control/, phase 3).
//
// FROZEN after the phase 3 kickoff: change only by agreement.
//
// Broadcast discovery stops at each floor's router, but plain UDP crosses it. So one laptop
// runs a directory (the lancall_directory program) and everyone else is told its IP.
//
// Every 2 s, each client's Discovery sends its normal beacon line straight to
// <directory_ip>:proto::kDirectoryPort (as well as broadcasting it on its floor):
//
//   LANCALL 1 id=3fa9c2d1 name=Aric sig=5011
//
// The directory answers every beacon with one datagram per OTHER peer it heard in the last
// 6 s, sent back to the address the beacon came from (the client's discovery socket):
//
//   PEER 1 id=7c01aa2e name=Atul ip=10.8.66.20 sig=5011
//
// ip is where the directory heard that peer from, never what a beacon claims. The directory
// keeps no state beyond that table, so restarting it is harmless: the table refills in 2 s.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/protocol.hpp"

namespace control {

class DirectoryServer {
public:
    DirectoryServer();
    ~DirectoryServer();  // stops if still running
    DirectoryServer(const DirectoryServer&) = delete;
    DirectoryServer& operator=(const DirectoryServer&) = delete;

    // false + error() if the port can't be bound
    bool start(uint16_t port = proto::kDirectoryPort);
    void stop();
    bool running() const;

    size_t peer_count() const;  // peers heard in the last 6 s. any thread
    std::string table() const;  // one "name ip port age" line per peer, for printing
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace control