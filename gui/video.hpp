#pragma once
// The pictures of lancall_gui's video panel (Person 2, gui/, phase 9.4): draws a call::Screen
// (call/app.hpp) the way the terminal's window does (call/view.hpp), scaled to the panel.
//
//   one picture: the other person (or the presenter, or our own broadcast) as big as fits,
//                our camera in the bottom-right corner
//   a grid:      one named tile per person in id order (3-4: 2x2, 5-6: 3x2)
//   status line along the top; "waiting for video" where a picture is missing
//
// Owns the textures (gui/texture.hpp). Main thread only; release() before the GL context goes.

#include <vector>

#include "call/app.hpp"
#include "gui/texture.hpp"

struct ImVec2;

namespace gui {

class VideoView {
public:
    // Draws s into the current ImGui window's area pos..pos+size (screen pixels). fps: the
    // camera's frame rate, how often our own picture is worth refreshing. missing: what the
    // big picture's place says while there's none ("waiting for video", "not in a call").
    void draw(const call::Screen& s, int fps, const char* missing, const ImVec2& pos,
              const ImVec2& size);
    void release();

private:
    Texture main_, corner_;
    std::vector<Texture> tiles_;
};

}  // namespace gui
