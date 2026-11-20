// Unit tests for call/commands.hpp (typed commands of the lancall app).

#include "call/commands.hpp"

#include <string>

#include "check.hpp"

using call::CommandKind;
using call::parse_command;

namespace {

void test_simple_commands() {
    CHECK(parse_command("help").kind == CommandKind::Help);
    CHECK(parse_command("?").kind == CommandKind::Help);
    CHECK(parse_command("stats").kind == CommandKind::Stats);
    CHECK(parse_command("list").kind == CommandKind::List);
    CHECK(parse_command("accept").kind == CommandKind::Accept);
    CHECK(parse_command("decline").kind == CommandKind::Decline);
    CHECK(parse_command("hangup").kind == CommandKind::Hangup);
    CHECK(parse_command("quit").kind == CommandKind::Quit);
    CHECK(parse_command("exit").kind == CommandKind::Quit);
}

void test_case_and_spaces() {
    CHECK(parse_command("  HANGUP  ").kind == CommandKind::Hangup);
    CHECK(parse_command("\tQuit\r").kind == CommandKind::Quit);
    const auto c = parse_command("  Call   Aric  ");
    CHECK(c.kind == CommandKind::Call);
    CHECK(c.arg == "Aric");  // the name keeps its case
}

void test_call_argument() {
    const auto ip = parse_command("call 10.8.66.20");
    CHECK(ip.kind == CommandKind::Call);
    CHECK(ip.arg == "10.8.66.20");

    const auto none = parse_command("call");
    CHECK(none.kind == CommandKind::Invalid);
    CHECK(none.error == "usage: call <name|ip>");

    const auto two = parse_command("call Aric Atul");
    CHECK(two.kind == CommandKind::Invalid);
}

void test_empty_and_invalid() {
    CHECK(parse_command("").kind == CommandKind::Empty);
    CHECK(parse_command("   \t ").kind == CommandKind::Empty);

    const auto unknown = parse_command("dial Aric");
    CHECK(unknown.kind == CommandKind::Invalid);
    CHECK(unknown.error == "unknown command 'dial' (type help)");

    const auto extra = parse_command("quit now");
    CHECK(extra.kind == CommandKind::Invalid);
    CHECK(extra.error == "'quit' takes no arguments");
}

void test_help_text_lists_every_command() {
    const std::string h = call::help_text();
    for (const char* word : {"help", "stats", "list", "call", "accept", "decline", "hangup", "quit",
                             "add", "members", "relay", "mesh", "text"}) {
        CHECK(h.find(word) != std::string::npos);
    }
}

// phase 4: group-call commands
void test_group_commands() {
    const auto add = parse_command("add Rishika");
    CHECK(add.kind == CommandKind::Add);
    CHECK(add.arg == "Rishika");
    CHECK(parse_command("ADD 10.8.66.31").arg == "10.8.66.31");
    CHECK(parse_command("add").kind == CommandKind::Invalid);
    CHECK(parse_command("add").error == "usage: add <name|ip>");

    CHECK(parse_command("members").kind == CommandKind::Members);
    CHECK(parse_command("members all").kind == CommandKind::Invalid);
    CHECK(parse_command("Mesh").kind == CommandKind::Mesh);

    const auto relay = parse_command("relay me");
    CHECK(relay.kind == CommandKind::Relay);
    CHECK(relay.arg == "me");
    CHECK(parse_command("relay 10.8.66.5").arg == "10.8.66.5");
    const auto bare = parse_command("relay");
    CHECK(bare.kind == CommandKind::Invalid);
    CHECK(bare.error == "usage: relay <ip|me>");
    CHECK(parse_command("relay a b").kind == CommandKind::Invalid);
}

// phase 6: text chat
void test_text_command() {
    const auto all = parse_command("text all hello   there, = everyone ");
    CHECK(all.kind == CommandKind::Text);
    CHECK(all.arg == "all");
    CHECK(all.text == "hello   there, = everyone");  // inner spaces kept, ends trimmed

    const auto one = parse_command("  TEXT  Atul   see you at 5\r");
    CHECK(one.kind == CommandKind::Text);
    CHECK(one.arg == "Atul");
    CHECK(one.text == "see you at 5");

    CHECK(parse_command("text 10.8.66.20 hi").arg == "10.8.66.20");
    CHECK(parse_command("text").kind == CommandKind::Invalid);
    CHECK(parse_command("text all").kind == CommandKind::Invalid);
    CHECK(parse_command("text all").error == "usage: text <all|name|ip> <message>");
}

// phase 7: broadcasts
void test_broadcast_commands() {
    const auto b = parse_command("  Broadcast   CN lecture:  routing, part 2  ");
    CHECK(b.kind == CommandKind::Broadcast);
    CHECK(b.text == "CN lecture:  routing, part 2");  // the rest of the line, ends trimmed
    CHECK(parse_command("broadcast x").text == "x");
    CHECK(parse_command("broadcast").kind == CommandKind::Invalid);
    CHECK(parse_command("broadcast   ").error == "usage: broadcast <title>");

    CHECK(parse_command("unbroadcast").kind == CommandKind::Unbroadcast);
    CHECK(parse_command("unbroadcast now").kind == CommandKind::Invalid);

    const auto w = parse_command("WATCH Aric");
    CHECK(w.kind == CommandKind::Watch);
    CHECK(w.arg == "Aric");
    CHECK(parse_command("watch 10.8.66.20").arg == "10.8.66.20");
    CHECK(parse_command("watch").error == "usage: watch <name|ip>");
    CHECK(parse_command("watch a b").kind == CommandKind::Invalid);
    CHECK(parse_command("unwatch").kind == CommandKind::Unwatch);

    const std::string h = call::help_text();
    for (const char* word : {"broadcast <title>", "unbroadcast", "watch <name|ip>", "unwatch"})
        CHECK(h.find(word) != std::string::npos);
}

}  // namespace

int main() {
    RUN(test_simple_commands);
    RUN(test_case_and_spaces);
    RUN(test_call_argument);
    RUN(test_empty_and_invalid);
    RUN(test_help_text_lists_every_command);
    RUN(test_group_commands);
    RUN(test_text_command);
    RUN(test_broadcast_commands);
    return check::result();
}
