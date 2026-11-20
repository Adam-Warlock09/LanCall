// Tests for call/app.hpp (phase 9): the checks every action makes and what it says, with no
// network, camera or mic. The App is never start()ed, so nothing is sent (no beacons) and
// no device is opened: these are the paths taken before a call, or when the control plane
// couldn't start. The texts are the ones terminal lancall prints (experiment scripts read
// them), so they're compared exactly. Lives in tests/metrics/ with Person 2's other tests.

#include "call/app.hpp"

#include <string>
#include <vector>

#include "call/commands.hpp"
#include "check.hpp"

namespace {

call::AppOptions options(const std::string& peer = "") {
    call::AppOptions o;
    o.name = "Tester";
    o.peer = peer;
    o.port = 25061;
    return o;
}

// what the last action said (one notice expected)
std::string said(call::App& app) {
    const std::vector<call::Notice> n = app.take_notices();
    if (n.size() != 1) return "<" + std::to_string(n.size()) + " notices>";
    return n[0].text;
}

void test_direct_mode() {
    call::App app(options("10.9.9.9"));
    CHECK(app.direct());
    CHECK(app.state() == control::CallState::Idle);
    app.relay("me");
    CHECK(said(app) == "direct mode (--peer): no relay");
    app.mesh();
    CHECK(said(app) == "direct mode (--peer): always direct");
    app.multicast();
    CHECK(said(app) == "direct mode (--peer): always direct");
    app.text("all", "hi");
    CHECK(said(app) == "direct mode (--peer): no call setup, so no chat");
    app.call("Aric");
    CHECK(said(app) == "direct mode (--peer): the call is set up at start-up");
    app.add("Aric");
    CHECK(said(app) == "direct mode (--peer): no group calls");
    app.broadcast("demo");
    CHECK(said(app) == "direct mode (--peer): no broadcasts");
    app.watch("Aric");
    CHECK(said(app) == "direct mode (--peer): no broadcasts");
    app.accept();
    CHECK(said(app) == "nobody is calling");
    app.hang_up();
    CHECK(said(app) == "no call");
    app.run(call::parse_command("members"));
    CHECK(said(app) == "direct mode: you and 10.9.9.9");
    app.run(call::parse_command("list"));
    CHECK(said(app) == "direct mode (--peer): no discovery");
    CHECK(app.members().empty());
    CHECK(app.peers().empty());
}

// relay / mesh / multicast before a call: the choice for calls we start
void test_idle_choices() {
    call::App app(options());
    CHECK(!app.direct());
    app.relay("me");
    CHECK(said(app) == "calls you start will use the relay on this laptop");
    app.relay("10.8.66.20");
    CHECK(said(app) == "calls you start will use the relay 10.8.66.20");
    app.relay("nowhere");
    CHECK(said(app) == "usage: relay <ip|me>");
    app.mesh();
    CHECK(said(app) == "calls you start will use mesh (everyone to everyone)");
    app.multicast();
    CHECK(said(app) ==
          "calls you start will use multicast, if everyone is on this floor (else a relay on "
          "this laptop)");
    CHECK(!app.is_host());
}

// nothing going on: every "undo" says there's nothing to undo
void test_nothing_to_stop() {
    call::App app(options());
    app.accept();
    CHECK(said(app) == "nobody is calling");
    app.decline();
    CHECK(said(app) == "nobody is calling");
    app.hang_up();
    CHECK(said(app) == "no call");
    app.unwatch();
    CHECK(said(app) == "not watching anything");
    app.unbroadcast();
    CHECK(said(app) == "not broadcasting");
    app.bridge("10.8.64.7");
    CHECK(said(app) == "start a broadcast first (broadcast <title>)");
    app.unbridge("10.8.64.7");
    CHECK(said(app) == "no bridge at 10.8.64.7");
    app.run(call::parse_command("stats"));
    CHECK(said(app) == "no call");
    CHECK(!app.in_call() && !app.broadcasting() && !app.watching());
    CHECK(app.stats().empty());
}

// the control plane isn't running: calls, chat and broadcasts say why they can't
void test_no_control_plane() {
    call::App app(options());
    app.call("Aric");
    CHECK(said(app).rfind("call setup unavailable", 0) == 0);
    app.add("Aric");
    CHECK(said(app).rfind("call setup unavailable", 0) == 0);
    app.text("all", "hi");
    CHECK(said(app).rfind("call setup unavailable", 0) == 0);
    app.watch("Aric");
    CHECK(said(app).rfind("broadcasts need discovery", 0) == 0);
    app.broadcast("demo");
    CHECK(said(app).rfind("broadcasts need discovery", 0) == 0);
    app.run(call::parse_command("list"));
    CHECK(said(app).rfind("discovery unavailable", 0) == 0);
}

void test_notices() {
    call::App app(options());
    CHECK(app.take_notices().empty());
    // help: one notice per line of the help text, no empty one at the end
    app.run(call::parse_command("help"));
    const std::vector<call::Notice> help = app.take_notices();
    size_t lines = 0;
    for (const char* p = call::help_text(); *p; ++p) lines += *p == '\n';
    CHECK(help.size() == lines);
    CHECK(!help.empty() && !help.back().text.empty());
    CHECK(help.front().kind == call::Notice::Kind::Info);
    CHECK(app.take_notices().empty());  // taken: gone

    app.run(call::parse_command("frobnicate"));
    CHECK(said(app) == "unknown command 'frobnicate' (type help)");
    app.run(call::parse_command("quit"));  // the front end's: App says nothing
    app.run(call::parse_command(""));
    CHECK(app.take_notices().empty());

    // several actions in a row: in order
    app.mesh();
    app.unwatch();
    const std::vector<call::Notice> two = app.take_notices();
    CHECK(two.size() == 2);
    CHECK(two.size() == 2 && two[1].text == "not watching anything");
}

void test_screen_when_idle() {
    call::App app(options());
    app.tick();  // nothing to do, nothing to say
    CHECK(app.take_notices().empty());
    const call::Screen s = app.screen();
    CHECK(!s.grid);
    CHECK(s.tiles.empty());
    CHECK(s.main.empty() && s.corner.empty());
    CHECK(s.status.size() == 1);
    CHECK(s.status.size() == 1 && s.status[0].find("Tester") != std::string::npos);
}

void test_shutdown_without_start() {
    call::App app(options());
    app.shutdown();
    app.shutdown();
    CHECK(app.take_notices().empty());
}

}  // namespace

int main() {
    RUN(test_direct_mode);
    RUN(test_idle_choices);
    RUN(test_nothing_to_stop);
    RUN(test_no_control_plane);
    RUN(test_notices);
    RUN(test_screen_when_idle);
    RUN(test_shutdown_without_start);
    return check::result();
}
