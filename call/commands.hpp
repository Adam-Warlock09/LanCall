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
//
// Command words are case-insensitive; the argument of `call` keeps its case.

#include <string>

namespace call {

enum class CommandKind { Empty, Help, Stats, List, Call, Accept, Decline, Hangup, Quit, Invalid };

struct Command {
    CommandKind kind = CommandKind::Empty;
    std::string arg;    // the peer for Call
    std::string error;  // why the line is Invalid, ready to print
};

Command parse_command(const std::string& line);

// The help text printed by `help`, one command per line.
const char* help_text();

}  // namespace call
