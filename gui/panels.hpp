#pragma once
// lancall_gui's panels (Person 2, gui/, phase 9). Each draws one fixed part of the window
// with Dear ImGui and turns clicks into call::App actions. App does the checking and says
// what happened in its notices, which end up in the messages panel.
//
//   +-----------+---------------------------+
//   | peers     | video (gui/video.hpp)     |
//   +-----------+                           |
//   | call      |                           |
//   +-----------+---------------------------+
//   | tabs: messages + command box, chat,   |
//   | stats, settings (gui/tabs.hpp)        |
//   +---------------------------------------+
//
// Main thread only, like everything that touches ImGui. No OS headers here (portable).

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "audio/pa_audio.hpp"
#include "call/app.hpp"
#include "gui/video.hpp"

struct ImVec2;

namespace gui {

// One line of the messages panel.
struct Line {
    enum class Kind { Info, Chat, Incoming, Typed };  // Typed: a command the person entered
    Kind kind = Kind::Info;
    std::string text;
};

// What the panels remember between frames (text fields, the message history).
struct Ui {
    static constexpr size_t kMaxLines = 500;
    std::deque<Line> lines;
    bool new_lines = false;  // scroll the messages to the end
    char call_ip[64] = "";
    char add_who[64] = "";
    char relay_ip[64] = "";
    char broadcast_title[128] = "";  // cut to 48 bytes when announced (clean_title)
    char bridge_ip[64] = "";
    char command[600] = "";  // a message is up to 500 bytes, plus "text <name> "
    bool quit = false;       // `quit` typed in the command box

    // phase 9.5, the tabs (gui/tabs.hpp)
    uint64_t chat_seen = 0;    // seq of the newest chat line looked at (for "unread")
    bool chat_new = false;     // scroll the chat to the end
    char chat_text[512] = "";  // at most 500 bytes are sent (the limit is checked)
    std::string chat_target;   // who the chat goes to ("all", a name or an IP)
    std::vector<std::string> stats;
    double stats_at = -1;  // ImGui time of the last refresh
    bool settings_loaded = false;
    call::MediaOptions settings;  // being edited, until Apply
    char directory[64] = "";
    std::vector<audio::AudioDevice> devices;
    bool devices_loaded = false;

    // the bottom panel's height, chosen by dragging its top edge or with Expand/Shrink;
    // 0 = the normal size. Kept within call::bottom_limits() when drawn, not saved
    float bottom_h = 0;
    float bottom_drag_from = 0;  // its height when the drag started
};

// A window with a fixed place: the layout is ours, not the person's to drag around.
// extra_flags: more ImGuiWindowFlags (e.g. no title bar).
void begin_panel(const char* title, const ImVec2& pos, const ImVec2& size, int extra_flags = 0);
// A text field's contents, trimmed of spaces at both ends.
std::string field(const char* buf);

// App's notices into the history (and the terminal, if started from one). true if someone
// started calling us (the window then asks for attention).
bool take_notices(call::App& app, Ui& ui);
void add_line(Ui& ui, Line::Kind kind, const std::string& text);

// Each panel at its place: pos/size in pixels of the window's drawing area.
void draw_peers(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size);
void draw_call(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size);
void draw_video(call::App& app, VideoView& view, const ImVec2& pos, const ImVec2& size);
// The messages tab's contents (inside gui/tabs.hpp's bottom panel).
void draw_messages_tab(call::App& app, Ui& ui);
// The "X is calling" popup, open while the call state is Ringing.
void draw_incoming(call::App& app);

// Before there's an App: asks for our name (and a directory). true once Start is pressed
// with usable values in opt.
bool draw_start(call::AppOptions& opt, const ImVec2& size);

}  // namespace gui
