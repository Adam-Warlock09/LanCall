// What lancall_gui offers where (Person 2, call/, phase 9). See ui_logic.hpp.
#include "call/ui_logic.hpp"

#include <algorithm>

#include "call/view.hpp"

namespace call {

bool is_broadcasting(const control::Peer& p) {
    return !p.remote && p.group != 0 && !p.broadcast_title.empty();
}

PeerActions peer_actions(const control::Peer& p, control::CallState state, bool is_host,
                         bool broadcasting, bool watching) {
    PeerActions a;
    const bool idle = state == control::CallState::Idle;
    const bool free = idle && !broadcasting;  // the camera and the media ports are ours
    a.call = free && !watching;
    a.add = state == control::CallState::InCall && is_host;
    a.watch = free && is_broadcasting(p);  // watching someone else: watch switches over
    return a;
}

std::string watch_target(const control::Peer& p, const std::vector<control::Peer>& peers) {
    int same_name = 0;
    for (const control::Peer& q : peers) same_name += q.name == p.name;
    return same_name > 1 ? p.ip : p.name;
}

std::string mode_label(control::MediaMode mode, const std::string& address) {
    switch (mode) {
        case control::MediaMode::Relay:
            return "relay " + address;
        case control::MediaMode::Multicast:
            return "multicast " + address;
        default:
            return "mesh";
    }
}

std::string chat_line_text(const ChatLine& c) {
    if (c.failed) return "not delivered to " + c.to + " (" + c.reason + "): " + c.text;
    const bool private_to_us = !c.mine && c.to == "you";
    return c.from + " -> " + c.to + (private_to_us ? " (private)" : "") + ": " + c.text;
}

std::vector<Recipient> chat_recipients(bool in_call, const std::vector<control::Member>& members,
                                       const std::vector<control::Peer>& peers) {
    // everyone we could name, members first (App's text() looks there first too)
    struct Someone {
        std::string name, ip;
        bool member, remote;
    };
    std::vector<Someone> all;
    for (const control::Member& m : members)
        if (!m.me && !m.ip.empty()) all.push_back({m.name, m.ip, true, false});
    for (const control::Peer& p : peers) {
        bool listed = false;  // already there as a member
        for (const Someone& s : all)
            listed = listed || (s.member && s.ip == p.ip && s.name == p.name);
        if (!listed) all.push_back({p.name, p.ip, false, p.remote});
    }

    std::vector<Recipient> out;
    if (in_call) out.push_back({"everyone in the call", "all"});
    for (const Someone& s : all) {
        int same_name = 0;
        for (const Someone& t : all) same_name += t.name == s.name;
        const std::string name = s.name.empty() ? s.ip : s.name;
        std::string label =
            name + (s.member ? "  (in the call)" : "") + (s.remote ? "  (other floor)" : "");
        if (same_name > 1) label += "  " + s.ip;
        out.push_back({label, same_name > 1 || s.name.empty() ? s.ip : s.name});
    }
    return out;
}

const char* state_name(control::CallState s) {
    switch (s) {
        case control::CallState::Calling:
            return "calling";
        case control::CallState::Ringing:
            return "ringing";
        case control::CallState::InCall:
            return "in a call";
        default:
            return "idle";
    }
}

Box fit(float w, float h, const Box& area) {
    if (w <= 0 || h <= 0 || area.w <= 0 || area.h <= 0) return {area.x, area.y, 0, 0};
    const float scale = std::min(area.w / w, area.h / h);
    Box b;
    b.w = w * scale;
    b.h = h * scale;
    b.x = area.x + (area.w - b.w) / 2;
    b.y = area.y + (area.h - b.h) / 2;
    return b;
}

Box ViewMap::to_area(int x, int y, int w, int h) const {
    const float s = view.w / static_cast<float>(kViewWidth);
    return {view.x + x * s, view.y + y * s, w * s, h * s};
}

ViewMap view_map(const Box& area) {
    return {fit(static_cast<float>(kViewWidth), static_cast<float>(kViewHeight), area)};
}

}  // namespace call
