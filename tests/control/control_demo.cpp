// control_demo — discovery + signaling with no audio or video (Person 1, tests/control/).
// For trying the control plane between two laptops before the lancall app is merged.
//
//   control_demo <name> [--port 5011]
//
// Then type a command and press Enter:
//   list                   peers heard on this floor
//   call <name|ip> [port]  call someone from the list, or any IP (works across floors)
//   accept | decline       answer an incoming call
//   hangup                 end or cancel the call
//   add <name|ip> [port]   phase 4, host only: ring one more person into the call
//   members                who's in the call, their ids and clock offsets
//   relay <ip> | mesh      phase 4, host only: how media should flow
//   quit
//
// While in a call it prints the clock offset and round trip every 2 s.
// Two copies on one laptop work too: give the second one --port 5013 (5012 is the
// directory's). For a group call on one laptop, ring 127.0.0.2, 127.0.0.3, ...: they're all
// this laptop, but the host needs a different IP per member.
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
        case CallEvent::Kind::Joined:
            return "JOINED";
        case CallEvent::Kind::Left:
            return "LEFT";
        case CallEvent::Kind::MediaChanged:
            return "MEDIA CHANGED";
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
        "hangup, add <name|ip>, members, relay <ip>, mesh, quit\n",
        name.c_str(), port);

    auto next_report = std::chrono::steady_clock::now();
    for (;;) {
        CallEvent e;
        while (sig.poll_event(e)) {
            std::printf("** %s  %s (%s) id %u%s%s\n", kind_name(e.kind), e.peer_name.c_str(),
                        e.peer_ip.c_str(), e.member_id,
                        e.reason.empty() ? "" : "  reason: ", e.reason.c_str());
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
        unsigned port_arg = 0;
        in >> cmd >> arg >> port_arg;

        // a name from the list, or an IP; an explicit port wins over the listed one
        auto resolve = [&](std::string& ip, uint16_t& to_port) {
            ip = arg;
            to_port = proto::kSignalPort;
            for (auto& peer : disc.peers())
                if (peer.name == arg) {
                    ip = peer.ip;
                    to_port = peer.signal_port;
                }
            if (port_arg > 0 && port_arg < 65536) to_port = static_cast<uint16_t>(port_arg);
        };

        if (cmd == "quit") {
            break;
        } else if (cmd == "list") {
            auto peers = disc.peers();
            if (peers.empty()) std::printf("   nobody heard yet (%s)\n", disc.error().c_str());
            for (auto& peer : peers)
                std::printf("   %-16s %-15s port %u\n", peer.name.c_str(), peer.ip.c_str(),
                            peer.signal_port);
        } else if (cmd == "call") {
            std::string ip;
            uint16_t to_port;
            resolve(ip, to_port);
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
        } else if (cmd == "add") {
            std::string ip;
            uint16_t to_port;
            resolve(ip, to_port);
            if (!sig.invite(ip, to_port))
                std::printf("   can't add '%s' (not the host, already in, or room full)\n",
                            arg.c_str());
            else
                std::printf("   ringing %s:%u ...\n", ip.c_str(), to_port);
        } else if (cmd == "members") {
            auto ms = sig.members();
            if (ms.empty()) std::printf("   not in a call\n");
            for (auto& m : ms) {
                uint32_t off = 0;
                bool have = sig.clock_offset(m.id, off);
                std::printf("   %u %-16s %-15s %s", m.id, m.name.c_str(),
                            m.me ? "(me)" : m.ip.c_str(), m.id == 1 ? "host" : "    ");
                if (have)
                    std::printf("  offset %+.3f ms\n", static_cast<int32_t>(off) / 1000.0);
                else
                    std::printf("  offset ?\n");
            }
            std::printf("   media: %s %s\n",
                        sig.media_mode() == MediaMode::Relay ? "relay" : "mesh",
                        sig.relay_ip().c_str());
        } else if (cmd == "relay") {
            sig.set_media(MediaMode::Relay, arg);
        } else if (cmd == "mesh") {
            sig.set_media(MediaMode::Mesh);
        } else if (!cmd.empty()) {
            std::printf(
                "   commands: list, call <name|ip> [port], accept, decline, hangup, "
                "add <name|ip> [port], members, relay <ip>, mesh, quit\n");
        }
    }
    return 0;  // destructors hang up and stop both
}