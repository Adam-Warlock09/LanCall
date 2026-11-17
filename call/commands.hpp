#pragma once
// Typed commands of the lancall app (Person 2, call/). Pure parsing, no side effects, so it
// is unit-tested on its own (tests/metrics/test_commands.cpp).
//
//   help | ?            list the commands
//   stats               one line per running media part, plus lip-sync counters
//   list                peers found by discovery
//   call <name|ip>      call a peer
//   accept | decline    answer an incoming call
//   hangup              end the call, keep the app running
//   quit | exit         end the call and the app
// phase 4, group calls:
//   add <name|ip>       the host rings one more person into the call
//   members             who is in the call, and how media flows
//   relay <ip|me>       the host sends everyone's media through a relay (me = this laptop)
//   mesh                the host switches back to everyone sending to everyone
//
// Command words are case-insensitive; the argument of `call`, `add` and `relay` keeps its case.

#include <string>

namespace call {

enum class CommandKind {
    Empty,
    Help,
    Stats,
    List,
    Call,
    Accept,
    Decline,
    Hangup,
    Quit,
    Add,
    Members,
    Relay,
    Mesh,
    Invalid
};

struct Command {
    CommandKind kind = CommandKind::Empty;
    std::string arg;    // the peer for Call and Add, the relay for Relay
    std::string error;  // why the line is Invalid, ready to print
};

Command parse_command(const std::string& line);

// The help text printed by `help`, one command per line.
const char* help_text();

}  // namespace call
