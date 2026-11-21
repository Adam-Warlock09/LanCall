#pragma once
// What lancall_gui offers where (Person 2, call/, phase 9): pure decisions behind its
// buttons, kept apart from the drawing (gui/) so they're unit-tested even on a laptop
// without GLFW (tests/metrics/test_front_end.cpp). App still checks everything again when
// a button is pressed and says why not, so these only decide what's worth showing.

#include <cstdint>
#include <string>
#include <vector>

#include "control/discovery.hpp"
#include "control/signaling.hpp"

namespace call {

// A peer on our floor announcing a broadcast in its beacon (other floors can't be watched).
bool is_broadcasting(const control::Peer& p);

// The buttons a peer's row gets.
struct PeerActions {
    bool call = false;   // we're idle and free: ring them
    bool add = false;    // we host a call: ring them into it
    bool watch = false;  // they broadcast on our floor and we're free to watch
};
PeerActions peer_actions(const control::Peer& p, control::CallState state, bool is_host,
                         bool broadcasting, bool watching);

// What `watch` needs to find p's broadcast: its name, or its IP when another peer has the
// same name (the IP then picks whichever of them broadcasts).
std::string watch_target(const control::Peer& p, const std::vector<control::Peer>& peers);

// "mesh", "relay 10.8.66.20", "multicast 239.255.77.4": how a call's media flows.
std::string mode_label(control::MediaMode mode, const std::string& address);

// The Call panel's heading while in a call: "Group call, N people" for 3 or more, else "In
// call with <the other member>" from the member list, which is always current (the person
// first called may have left a group call since). Without a member list (direct mode, or
// before signaling has one) it falls back to peer_name, then peer_ip.
std::string call_heading(const std::vector<control::Member>& members, const std::string& peer_name,
                         const std::string& peer_ip);

// ---- phase 9.5: chat and stats --------------------------------------------------------------

// One chat message, for the GUI's chat tab. call::App keeps the last ones in memory only (the
// CSV log gets counts, never the text).
struct ChatLine {
    uint64_t seq = 0;  // 1, 2, 3, ... in order (for "unread")
    std::string time;  // local "HH:MM"
    std::string from;  // a name or IP; "you" for ours
    std::string to;    // "everyone", "you", or a name or IP
    std::string text;
    bool mine = false;
    bool failed = false;  // ours, not delivered (reason says why)
    std::string reason;
};

// How the chat tab shows it: "Aric -> you (private): hi", "you -> everyone: hi",
// "not delivered to 10.8.66.20 (no answer): hi".
std::string chat_line_text(const ChatLine& c);

// Who the chat tab can write to: everyone (in a call), the call's members, then the other
// peers. target is what App::text() takes: "all", a name, or an IP when names repeat.
struct Recipient {
    std::string label;
    std::string target;
};
std::vector<Recipient> chat_recipients(bool in_call, const std::vector<control::Member>& members,
                                       const std::vector<control::Peer>& peers);

// "idle", "calling", "ringing", "in a call"
const char* state_name(control::CallState s);

// ---- the video panel's geometry (phase 9.4), in pixels ------------------------------------

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
};

// The largest box with the picture's shape (w x h) that fits in area, centred in it.
Box fit(float w, float h, const Box& area);

// The terminal window's layout (call/view.hpp: kViewWidth x kViewHeight, 4:3) placed into
// area: view is that whole view fitted in, and to_area() maps one of its rectangles (a grid
// tile, the self-view inset) into it. So the GUI shows the same arrangement, just bigger.
struct ViewMap {
    Box view;
    Box to_area(int x, int y, int w, int h) const;
};
ViewMap view_map(const Box& area);

}  // namespace call
