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
// phase 6, text chat:
//   text all <message>        to everyone in the call
//   text <name|ip> <message>  to one person, in the call or not
//
// Command words are case-insensitive; the argument of `call`, `add` and `relay` keeps its case.
// A message is the rest of the line after the target, as typed (inner spaces kept).

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
    Text,
    Invalid
};

struct Command {
    CommandKind kind = CommandKind::Empty;
    std::string arg;    // the peer for Call and Add, the relay for Relay, "all"/peer for Text
    std::string text;   // the message for Text
    std::string error;  // why the line is Invalid, ready to print
};

Command parse_command(const std::string& line);

// The help text printed by `help`, one command per line.
const char* help_text();

}  // namespace call
