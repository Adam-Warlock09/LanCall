// Tests for the pieces both front ends share (phase 9): lancall's command-line options
// (call/app_args.hpp) and what lancall_gui's buttons offer (call/ui_logic.hpp). No network,
// no devices, no window. Lives in tests/metrics/ with Person 2's other tests.

#include <string>
#include <vector>

#include "call/app.hpp"
#include "call/app_args.hpp"
#include "call/ui_logic.hpp"
#include "call/view.hpp"
#include "check.hpp"

namespace {

using control::CallState;

// parse_app_args on a command line given as words (argv[0] added)
bool parse(std::vector<std::string> words, call::AppOptions& o, std::vector<std::string>& msgs,
           bool name_required = true) {
    words.insert(words.begin(), "lancall");
    std::vector<char*> argv;
    for (std::string& w : words) argv.push_back(w.data());
    return call::parse_app_args(static_cast<int>(argv.size()), argv.data(), o, msgs, name_required);
}

void test_args_all_options() {
    call::AppOptions o;
    std::vector<std::string> msgs;
    CHECK(parse({"--name",    "Atul",  "--directory", "10.8.64.5", "--relay", "me",
                 "--log",     "x.csv", "--in",        "3",         "--out",   "4",
                 "--camera",  "1",     "--buffer-ms", "80",        "--fps",   "10",
                 "--quality", "60",    "--fec",       "--port",    "5013",    "--mc-loop"},
                o, msgs));
    CHECK(msgs.empty());
    CHECK(o.name == "Atul");
    CHECK(o.directory == "10.8.64.5");
    CHECK(o.relay == "me");
    CHECK(o.log_path == "x.csv");
    CHECK(o.media.in_device == 3 && o.media.out_device == 4 && o.media.camera == 1);
    CHECK(o.media.buffer_ms == 80 && o.media.fps == 10 && o.media.quality == 60);
    CHECK(o.media.fec);
    CHECK(o.port == 5013);
    CHECK(o.mc_loop);
    CHECK(o.peer.empty());
}

void test_args_defaults() {
    call::AppOptions o;
    std::vector<std::string> msgs;
    CHECK(parse({"--name", "A"}, o, msgs));
    CHECK(msgs.empty());
    CHECK(o.port == proto::kSignalPort);
    CHECK(o.media.fps == 15 && o.media.quality == 70 && o.media.buffer_ms == 60);
    CHECK(!o.media.fec && !o.mc_loop);
}

void test_args_refused() {
    call::AppOptions o;
    std::vector<std::string> msgs;
    CHECK(!parse({}, o, msgs));  // no name
    CHECK(msgs.size() == 1 && msgs[0] == "--name: use 1-32 letters, digits, '-' or '_'");

    msgs.clear();
    CHECK(!parse({"--name", "bad name!"}, o, msgs));
    msgs.clear();
    o = {};
    CHECK(!parse({"--name", "A", "--fps", "99"}, o, msgs));  // out of range: usage only
    CHECK(msgs.empty());
    o = {};
    CHECK(!parse({"--name", "A", "--what"}, o, msgs));
    o = {};
    CHECK(!parse({"--name"}, o, msgs));  // no value
    o = {};
    msgs.clear();
    CHECK(!parse({"--name", "A", "--peer", "bogus"}, o, msgs));
    CHECK(msgs.size() == 1 &&
          msgs[0] == "--peer: give the other laptop's IPv4 address (or 127.0.0.1)");
    o = {};
    msgs.clear();
    CHECK(!parse({"--name", "A", "--directory", "floor2"}, o, msgs));
    CHECK(msgs.size() == 1 &&
          msgs[0] == "--directory: give the IPv4 address of the lancall_directory laptop");
    o = {};
    msgs.clear();
    CHECK(!parse({"--name", "A", "--relay", "there"}, o, msgs));
    CHECK(msgs.size() == 1 && msgs[0] == "--relay: give the relay laptop's IPv4 address, or me");
}

// options that do nothing in direct mode: accepted, with a word about it each
void test_args_warnings() {
    call::AppOptions o;
    std::vector<std::string> msgs;
    CHECK(parse({"--name", "A", "--peer", "127.0.0.1", "--directory", "10.8.64.5", "--relay", "me"},
                o, msgs));
    CHECK(msgs.size() == 2);
    CHECK(msgs.size() == 2 &&
          msgs[0] == "--directory has no effect with --peer (direct mode has no discovery)");
    CHECK(msgs.size() == 2 &&
          msgs[1] == "--relay has no effect with --peer (direct mode is always direct)");
}

// lancall_gui: the name may wait for its start screen, but one that's given must be valid
void test_args_name_optional() {
    call::AppOptions o;
    std::vector<std::string> msgs;
    CHECK(parse({"--directory", "10.8.64.5"}, o, msgs, false));
    CHECK(msgs.empty() && o.name.empty());
    o = {};
    CHECK(!parse({"--name", "no spaces please"}, o, msgs, false));
}

control::Peer peer(const std::string& name, const std::string& ip, uint8_t group = 0,
                   const std::string& title = "", bool remote = false) {
    control::Peer p;
    p.name = name;
    p.ip = ip;
    p.signal_port = proto::kSignalPort;
    p.group = group;
    p.broadcast_title = title;
    p.remote = remote;
    return p;
}

void test_is_broadcasting() {
    CHECK(call::is_broadcasting(peer("A", "10.0.0.1", 7, "demo")));
    CHECK(!call::is_broadcasting(peer("A", "10.0.0.1", 7, "")));  // a multicast call's group
    CHECK(!call::is_broadcasting(peer("A", "10.0.0.1", 0, "demo")));
    CHECK(!call::is_broadcasting(peer("A", "10.0.0.1", 7, "demo", true)));  // other floor
}

void test_peer_actions() {
    const control::Peer plain = peer("A", "10.0.0.1");
    const control::Peer live = peer("B", "10.0.0.2", 7, "demo");

    call::PeerActions a = call::peer_actions(plain, CallState::Idle, false, false, false);
    CHECK(a.call && !a.add && !a.watch);
    a = call::peer_actions(live, CallState::Idle, false, false, false);
    CHECK(a.call && !a.add && a.watch);

    // we broadcast: the camera is taken, nothing to start
    a = call::peer_actions(live, CallState::Idle, false, true, false);
    CHECK(!a.call && !a.add && !a.watch);
    // we watch: no calls, but watching someone else switches over
    a = call::peer_actions(live, CallState::Idle, false, false, true);
    CHECK(!a.call && a.watch);

    // in a call: the host adds, nobody calls or watches
    a = call::peer_actions(live, CallState::InCall, true, false, false);
    CHECK(!a.call && a.add && !a.watch);
    a = call::peer_actions(live, CallState::InCall, false, false, false);
    CHECK(!a.call && !a.add && !a.watch);
    // while a call is being set up: nothing
    a = call::peer_actions(plain, CallState::Calling, false, false, false);
    CHECK(!a.call && !a.add && !a.watch);
    a = call::peer_actions(plain, CallState::Ringing, false, false, false);
    CHECK(!a.call && !a.add && !a.watch);
}

void test_watch_target() {
    const std::vector<control::Peer> peers = {peer("A", "10.0.0.1", 7, "demo"),
                                              peer("B", "10.0.0.2"), peer("B", "10.0.0.3", 9, "x")};
    CHECK(call::watch_target(peers[0], peers) == "A");
    CHECK(call::watch_target(peers[2], peers) == "10.0.0.3");  // two Bs: by IP
}

void test_mode_label() {
    CHECK(call::mode_label(control::MediaMode::Mesh, "") == "mesh");
    CHECK(call::mode_label(control::MediaMode::Relay, "10.8.66.20") == "relay 10.8.66.20");
    CHECK(call::mode_label(control::MediaMode::Multicast, "239.255.77.4") ==
          "multicast 239.255.77.4");
}

// ---- phase 9.4: the video panel's geometry -------------------------------------------------

void test_fit() {
    // a 4:3 picture in a wide box: full height, centred sideways
    call::Box b = call::fit(640, 480, {0, 0, 1000, 500});
    CHECK_NEAR(b.h, 500, 1e-3);
    CHECK_NEAR(b.w, 666.667, 1e-2);
    CHECK_NEAR(b.x, 166.667, 1e-2);
    CHECK_NEAR(b.y, 0, 1e-3);
    // in a tall box: full width, centred up and down (offsets kept)
    b = call::fit(4, 3, {10, 20, 400, 600});
    CHECK_NEAR(b.w, 400, 1e-3);
    CHECK_NEAR(b.h, 300, 1e-3);
    CHECK_NEAR(b.x, 10, 1e-3);
    CHECK_NEAR(b.y, 170, 1e-3);
    // nothing to fit, or nowhere: an empty box
    CHECK(call::fit(0, 480, {0, 0, 100, 100}).w == 0);
    CHECK(call::fit(640, 480, {0, 0, 0, 100}).w == 0);
}

void test_view_map() {
    // exactly twice the terminal's 640x480: every rectangle doubles
    const call::ViewMap m = call::view_map({0, 0, 1280, 960});
    CHECK_NEAR(m.view.w, 1280, 1e-3);
    const cv::Rect r = call::inset_rect();
    const call::Box inset = m.to_area(r.x, r.y, r.width, r.height);
    CHECK_NEAR(inset.x, 2.0 * r.x, 1e-3);
    CHECK_NEAR(inset.y, 2.0 * r.y, 1e-3);
    CHECK_NEAR(inset.w, 2.0 * r.width, 1e-3);
    CHECK_NEAR(inset.h, 2.0 * r.height, 1e-3);

    // a wide panel with an offset: the 4:3 view is centred, the grid stays inside it
    const call::Box area{100, 50, 1600, 600};
    const call::ViewMap w = call::view_map(area);
    CHECK_NEAR(w.view.h, 600, 1e-3);
    CHECK_NEAR(w.view.w, 800, 1e-3);
    CHECK_NEAR(w.view.x, 100 + 400, 1e-3);
    for (const cv::Rect& g : call::grid_rects(5)) {
        const call::Box t = w.to_area(g.x, g.y, g.width, g.height);
        CHECK(t.x >= w.view.x - 1e-3 && t.x + t.w <= w.view.x + w.view.w + 1e-3);
        CHECK(t.y >= w.view.y - 1e-3 && t.y + t.h <= w.view.y + w.view.h + 1e-3);
    }
}

// the line `bridge` prints, and the GUI's "Copy command" puts on the clipboard
void test_bridge_command() {
    call::AppOptions o;
    o.name = "Atul";
    call::App app(o);  // never started: nothing is sent
    CHECK(app.bridge_command("127.0.0.1") ==
          "./build/net/lancall_bridge --from 127.0.0.1 --name Atul --title \"\"");
    CHECK(app.bridges().empty());
}

}  // namespace

int main() {
    RUN(test_args_all_options);
    RUN(test_args_defaults);
    RUN(test_args_refused);
    RUN(test_args_warnings);
    RUN(test_args_name_optional);
    RUN(test_is_broadcasting);
    RUN(test_peer_actions);
    RUN(test_watch_target);
    RUN(test_mode_label);
    RUN(test_fit);
    RUN(test_view_map);
    RUN(test_bridge_command);
    return check::result();
}
