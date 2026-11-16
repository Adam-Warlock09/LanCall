// control_demo — discovery + signaling with no audio or video (Person 1, tests/control/).
// For trying the control plane between two laptops before the lancall app is merged.
//
//   control_demo <name> [--port 5011]
//
// Then type a command and press Enter:
//   list               peers heard on this floor
//   call <name|ip>     call someone from the list, or any IP (works across floors)
//   accept | decline   answer an incoming call
//   hangup             end or cancel the call
//   quit
//
// While in a call it prints the clock offset and round trip every 2 s.
// Two copies on one laptop work too: give the second one --port 5012.
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/signaling.hpp"

using namespace control;

static const char* kind_name(CallEvent::Kind k) {
    switch (k) {
        case CallEvent::Kind::Incoming:
            return "INCOMING CALL";
        case CallEvent::Kind::Accepted:
            return "CALL UP";
        case CallEvent::Kind::Declined:
            return "DECLINED";
        case CallEvent::Kind::Ended:
            return "CALL ENDED";
        case CallEvent::Kind::Failed:
            return "CALL FAILED";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <name> [--port 5011]\n", argv[0]);
        return 2;
    }
    std::string name = argv[1];
    uint16_t port = proto::kSignalPort;
    if (argc >= 4 && std::string(argv[2]) == "--port")
        port = static_cast<uint16_t>(std::stoi(argv[3]));

    Discovery disc;
    Signaling sig;
    if (!sig.start(name, port)) {
        std::fprintf(stderr, "signaling: %s\n", sig.error().c_str());
        return 1;
    }
    if (!disc.start(name, port)) {
        std::fprintf(stderr, "discovery: %s\n", disc.error().c_str());
        return 1;
    }
    std::printf(
        "%s ready, calls on TCP %u. commands: list, call <name|ip>, accept, decline, "
        "hangup, quit\n",
        name.c_str(), port);

    auto next_report = std::chrono::steady_clock::now();
    for (;;) {
        CallEvent e;
        while (sig.poll_event(e)) {
            std::printf("** %s  %s (%s)%s%s\n", kind_name(e.kind), e.peer_name.c_str(),
                        e.peer_ip.c_str(), e.reason.empty() ? "" : "  reason: ", e.reason.c_str());
            if (e.kind == CallEvent::Kind::Incoming) std::printf("   type accept or decline\n");
        }

        if (sig.state() == CallState::InCall && std::chrono::steady_clock::now() >= next_report) {
            uint32_t off;
            if (sig.clock_offset(off))
                std::printf("   clock: peer - ours = %.3f s (mod 2^32 us), rtt %.2f ms\n",
                            off / 1e6, sig.rtt_ms());
            next_report = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }

        // wait up to 100 ms for a line on stdin, so events keep printing meanwhile
        pollfd p{STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        std::string line;
        if (!std::getline(std::cin, line)) break;  // ctrl+d
        std::istringstream in(line);
        std::string cmd, arg;
        in >> cmd >> arg;

        if (cmd == "quit") {
            break;
        } else if (cmd == "list") {
            auto peers = disc.peers();
            if (peers.empty()) std::printf("   nobody heard yet (%s)\n", disc.error().c_str());
            for (auto& peer : peers)
                std::printf("   %-16s %-15s port %u\n", peer.name.c_str(), peer.ip.c_str(),
                            peer.signal_port);
        } else if (cmd == "call") {
            std::string ip = arg;
            uint16_t to_port = proto::kSignalPort;
            for (auto& peer : disc.peers())
                if (peer.name == arg) {
                    ip = peer.ip;
                    to_port = peer.signal_port;
                }
            if (!sig.call(ip, to_port))
                std::printf("   can't call '%s' (not idle, or not a name/IP)\n", arg.c_str());
            else
                std::printf("   calling %s:%u ...\n", ip.c_str(), to_port);
        } else if (cmd == "accept") {
            sig.accept();
        } else if (cmd == "decline") {
            sig.decline();
        } else if (cmd == "hangup") {
            sig.hang_up();
        } else if (!cmd.empty()) {
            std::printf("   commands: list, call <name|ip>, accept, decline, hangup, quit\n");
        }
    }
    return 0;  // destructors hang up and stop both
}