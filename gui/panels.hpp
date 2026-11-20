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
//   | messages + command box                |
//   +---------------------------------------+
//
// Main thread only, like everything that touches ImGui. No OS headers here (portable).

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

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
};

// App's notices into the history (and the terminal, if started from one). true if someone
// started calling us (the window then asks for attention).
bool take_notices(call::App& app, Ui& ui);
void add_line(Ui& ui, Line::Kind kind, const std::string& text);

// Each panel at its place: pos/size in pixels of the window's drawing area.
void draw_peers(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size);
void draw_call(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size);
void draw_video(call::App& app, VideoView& view, const ImVec2& pos, const ImVec2& size);
void draw_messages(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size);
// The "X is calling" popup, open while the call state is Ringing.
void draw_incoming(call::App& app);

// Before there's an App: asks for our name (and a directory). true once Start is pressed
// with usable values in opt.
bool draw_start(call::AppOptions& opt, const ImVec2& size);

}  // namespace gui
