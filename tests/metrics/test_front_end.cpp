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

// ---- phase 9.5: chat, stats, settings ------------------------------------------------------

void test_chat_line_text() {
    call::ChatLine c;
    c.from = "Aric";
    c.to = "you";
    c.text = "hi";
    CHECK(call::chat_line_text(c) == "Aric -> you (private): hi");
    c.to = "everyone";
    CHECK(call::chat_line_text(c) == "Aric -> everyone: hi");
    c.from = "you";
    c.to = "Rishika";
    c.mine = true;
    CHECK(call::chat_line_text(c) == "you -> Rishika: hi");
    c.to = "10.8.66.20";
    c.failed = true;
    c.reason = "no answer";
    CHECK(call::chat_line_text(c) == "not delivered to 10.8.66.20 (no answer): hi");
}

control::Member member(uint8_t id, const std::string& name, const std::string& ip, bool me) {
    control::Member m;
    m.id = id;
    m.name = name;
    m.ip = ip;
    m.me = me;
    return m;
}

void test_chat_recipients() {
    // not in a call: just the peers, by name
    std::vector<call::Recipient> r =
        call::chat_recipients(false, {}, {peer("Aric", "10.0.0.1"), peer("Rishika", "10.0.0.2")});
    CHECK(r.size() == 2);
    CHECK(r.size() == 2 && r[0].target == "Aric" && r[1].target == "Rishika");

    // in a call: everyone first, then the members (not us), then the other peers; a peer
    // who is also a member is listed once
    const std::vector<control::Member> members = {member(1, "Atul", "10.0.0.9", true),
                                                  member(2, "Aric", "10.0.0.1", false)};
    r = call::chat_recipients(true, members,
                              {peer("Aric", "10.0.0.1"), peer("Rishika", "10.0.0.2", 0, "", true)});
    CHECK(r.size() == 3);
    if (r.size() == 3) {
        CHECK(r[0].target == "all");
        CHECK(r[1].target == "Aric" && r[1].label.find("in the call") != std::string::npos);
        CHECK(r[2].target == "Rishika" && r[2].label.find("other floor") != std::string::npos);
    }

    // two with one name: by IP
    r = call::chat_recipients(false, {}, {peer("B", "10.0.0.2"), peer("B", "10.0.0.3")});
    CHECK(r.size() == 2 && r[0].target == "10.0.0.2" && r[1].target == "10.0.0.3");
}

void test_state_name() {
    CHECK(std::string(call::state_name(CallState::Idle)) == "idle");
    CHECK(std::string(call::state_name(CallState::Calling)) == "calling");
    CHECK(std::string(call::state_name(CallState::Ringing)) == "ringing");
    CHECK(std::string(call::state_name(CallState::InCall)) == "in a call");
}

// the settings tab checks with the command line's limits
void test_media_options_problem() {
    call::MediaOptions m;
    CHECK(call::media_options_problem(m).empty());  // the defaults are fine
    m.fps = 31;
    CHECK(call::media_options_problem(m) == "frame rate: 1-30");
    m = {};
    m.buffer_ms = 10;
    CHECK(call::media_options_problem(m) == "audio buffer: 20-600 ms");
    m = {};
    m.quality = 0;
    CHECK(call::media_options_problem(m) == "quality: 1-100");
    m = {};
    m.camera = 64;
    CHECK(call::media_options_problem(m) == "camera: 0-63");
    m = {};
    m.in_device = -2;
    CHECK(call::media_options_problem(m) == "microphone: no such device");
    m = {};
    m.out_device = 7;  // a device number: fine (whether it exists shows when it opens)
    CHECK(call::media_options_problem(m).empty());
}

// the Call panel's heading follows the member list, not the person first called
void test_call_heading() {
    const control::Member me = member(1, "Rishika", "10.0.0.9", true);
    const control::Member atul = member(2, "Atul", "10.0.0.1", false);
    const control::Member aric = member(3, "Aric", "10.0.0.2", false);

    // two people
    CHECK(call::call_heading({me, atul}, "Atul", "10.0.0.1") == "In call with Atul");
    // Atul (called first) left a 3-person call: the heading names who's really there
    CHECK(call::call_heading({me, aric}, "Atul", "10.0.0.1") == "In call with Aric");
    // three or more
    CHECK(call::call_heading({me, atul, aric}, "Atul", "10.0.0.1") == "Group call, 3 people");
    // no member list (direct mode): the peer's name, else its IP
    CHECK(call::call_heading({}, "", "127.0.0.1") == "In call with 127.0.0.1");
    CHECK(call::call_heading({}, "Atul", "10.0.0.1") == "In call with Atul");
    // a member without a name shows by IP
    CHECK(call::call_heading({me, member(2, "", "10.0.0.7", false)}, "", "") ==
          "In call with 10.0.0.7");
}

// the bottom panel's height: dragged or expanded, within limits that follow the window
void test_bottom_height() {
    // an 800 px window: normal is a quarter (as before it could change), at most 70%
    call::BottomLimits l = call::bottom_limits(800);
    CHECK_NEAR(l.min, 150, 1e-3);
    CHECK_NEAR(l.normal, 208, 1e-3);
    CHECK_NEAR(l.max, 560, 1e-3);
    CHECK_NEAR(call::bottom_height(800, 0), 208, 1e-3);  // not chosen: normal
    CHECK_NEAR(call::bottom_height(800, -5), 208, 1e-3);
    CHECK_NEAR(call::bottom_height(800, 400), 400, 1e-3);  // dragged: kept
    CHECK_NEAR(call::bottom_height(800, 100), 150, 1e-3);  // too short: the tabs still fit
    CHECK_NEAR(call::bottom_height(800, 5000), 560, 1e-3);
    // a big window: normal stops at 300, max at 70%
    l = call::bottom_limits(1200);
    CHECK_NEAR(l.normal, 300, 1e-3);
    CHECK_NEAR(l.max, 840, 1e-3);
    // Expand asks for the window's height (800 here): the window grows, the panel follows
    CHECK_NEAR(call::bottom_height(1200, 800), 800, 1e-3);
    CHECK_NEAR(call::bottom_height(1200, 1000), 840, 1e-3);
    // a short window: the top keeps 220 px before 70% matters
    l = call::bottom_limits(500);
    CHECK_NEAR(l.max, 280, 1e-3);
    CHECK_NEAR(l.normal, 150, 1e-3);
    // too small for both: the panel keeps its smallest size, never less
    l = call::bottom_limits(300);
    CHECK_NEAR(l.max, 150, 1e-3);
    CHECK_NEAR(l.normal, 150, 1e-3);
    CHECK_NEAR(call::bottom_height(300, 250), 150, 1e-3);
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
    RUN(test_chat_line_text);
    RUN(test_chat_recipients);
    RUN(test_state_name);
    RUN(test_media_options_problem);
    RUN(test_call_heading);
    RUN(test_bottom_height);
    return check::result();
}
