#pragma once
// The bottom panel of lancall_gui (Person 2, gui/, phase 9.5): four tabs.
//
//   Messages  everything App says, and a box for any lancall command (gui/panels.hpp)
//   Chat      the conversation, and writing to everyone in the call or one person
//   Stats     the `stats` lines, live (twice a second), with Copy
//   Settings  microphone, speaker, camera, buffer, fps, quality, FEC (from the next call),
//             and the directory laptop (when idle)
//
// "Chat (2)": unread messages while another tab is open. Main thread only; portable.
// Its height is the person's: drag the bar along its top edge, or Expand / Shrink at the
// right of the tabs; within call::bottom_limits() (call/ui_logic.hpp).

#include "call/app.hpp"
#include "gui/panels.hpp"

struct ImVec2;

namespace gui {

void draw_bottom(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size);

}  // namespace gui
