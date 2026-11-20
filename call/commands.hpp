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
//   multicast           (phase 7) the host makes everyone send one copy to a multicast group,
//                       when every member is on the host's floor
// phase 6, text chat:
//   text all <message>        to everyone in the call
//   text <name|ip> <message>  to one person, in the call or not
// phase 7, broadcasts on this floor:
//   broadcast <title>   send our camera and mic to everyone on the floor who watches
//   unbroadcast         stop it
//   watch <name|ip>     watch someone's broadcast (see list)
//   unwatch             stop watching
//
// Command words are case-insensitive; the argument of `call`, `add` and `relay` keeps its case.
// A message is the rest of the line after the target, as typed (inner spaces kept); so is a
// broadcast's title, the rest of the line after `broadcast`.

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
    Multicast,
    Text,
    Broadcast,
    Unbroadcast,
    Watch,
    Unwatch,
    Invalid
};

struct Command {
    CommandKind kind = CommandKind::Empty;
    std::string arg;   // the peer for Call, Add and Watch, the relay for Relay, "all"/peer for Text
    std::string text;  // the message for Text, the title for Broadcast
    std::string error;  // why the line is Invalid, ready to print
};

Command parse_command(const std::string& line);

// The help text printed by `help`, one command per line.
const char* help_text();

}  // namespace call
