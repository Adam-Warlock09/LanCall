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
