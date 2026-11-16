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
    for (const char* word :
         {"help", "stats", "list", "call", "accept", "decline", "hangup", "quit"}) {
        CHECK(h.find(word) != std::string::npos);
    }
}

}  // namespace

int main() {
    RUN(test_simple_commands);
    RUN(test_case_and_spaces);
    RUN(test_call_argument);
    RUN(test_empty_and_invalid);
    RUN(test_help_text_lists_every_command);
    return check::result();
}
