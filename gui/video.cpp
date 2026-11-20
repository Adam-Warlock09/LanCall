// The pictures of lancall_gui's video panel (Person 2, gui/, phase 9.4). See video.hpp.
#include "gui/video.hpp"

#include <algorithm>
#include <string>

#include "call/ui_logic.hpp"
#include "call/view.hpp"
#include "imgui.h"

namespace gui {

namespace {

const ImU32 kBackground = IM_COL32(16, 17, 20, 255);
const ImU32 kCard = IM_COL32(40, 42, 48, 255);
const ImU32 kFrame = IM_COL32(200, 200, 200, 255);
const ImU32 kStrip = IM_COL32(0, 0, 0, 150);
const ImU32 kText = IM_COL32(235, 235, 235, 255);
const ImU32 kDim = IM_COL32(150, 150, 150, 255);

ImVec2 tl(const call::Box& b) {
    return ImVec2(b.x, b.y);
}
ImVec2 br(const call::Box& b) {
    return ImVec2(b.x + b.w, b.y + b.h);
}

// text centred in b
void centred(ImDrawList* dl, const call::Box& b, const char* s, ImU32 color) {
    const ImVec2 t = ImGui::CalcTextSize(s);
    dl->AddText(ImVec2(b.x + (b.w - t.x) / 2, b.y + (b.h - t.y) / 2), color, s);
}

// the picture in tex, as big as fits in b with its own shape; a card saying `missing` if none
void picture(ImDrawList* dl, const Texture& tex, const call::Box& b, const char* missing) {
    if (tex.empty()) {
        dl->AddRectFilled(tl(b), br(b), kCard);
        centred(dl, b, missing, kDim);
        return;
    }
    const call::Box p =
        call::fit(static_cast<float>(tex.width()), static_cast<float>(tex.height()), b);
    dl->AddImage(static_cast<ImTextureID>(tex.id()), tl(p), br(p));
}

// a name on a dark strip at the bottom left of b
void label(ImDrawList* dl, const call::Box& b, const std::string& s) {
    const ImVec2 t = ImGui::CalcTextSize(s.c_str());
    const float pad = 4;
    const ImVec2 a(b.x, b.y + b.h - t.y - 2 * pad);
    dl->AddRectFilled(a, ImVec2(b.x + std::min(b.w, t.x + 2 * pad), b.y + b.h), kStrip);
    dl->AddText(ImVec2(a.x + pad, a.y + pad), kText, s.c_str());
}

}  // namespace

void VideoView::draw(const call::Screen& s, int fps, const char* missing, const ImVec2& pos,
                     const ImVec2& size) {
    const double self_every = 1.0 / std::max(1, fps);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const call::Box area{pos.x, pos.y, size.x, size.y};
    dl->AddRectFilled(tl(area), br(area), kBackground);
    const call::ViewMap map = call::view_map(area);

    if (s.grid) {
        tiles_.resize(s.tiles.size());
        const std::vector<cv::Rect> rects = call::grid_rects(static_cast<int>(s.tiles.size()));
        for (size_t i = 0; i < s.tiles.size() && i < rects.size(); ++i) {
            const bool mine = static_cast<int>(i) == s.self_tile;
            tiles_[i].show(s.tiles[i].image, mine ? self_every : 0);
            const cv::Rect& r = rects[i];
            const call::Box b = map.to_area(r.x, r.y, r.width, r.height);
            picture(dl, tiles_[i], b, "no video");
            label(dl, b, s.tiles[i].label);
        }
        main_.show(cv::Mat());
        corner_.show(cv::Mat());
    } else {
        tiles_.clear();
        main_.show(s.main, s.main_is_self ? self_every : 0);
        corner_.show(s.corner, self_every);
        picture(dl, main_, map.view, missing);
        if (!corner_.empty()) {
            const cv::Rect r = call::inset_rect();
            const call::Box b = map.to_area(r.x, r.y, r.width, r.height);
            picture(dl, corner_, b, "");
            dl->AddRect(ImVec2(b.x - 1, b.y - 1), ImVec2(b.x + b.w + 1, b.y + b.h + 1), kFrame);
        }
    }

    // the status line(s), top left, readable on any picture
    float y = map.view.y + 6;
    for (const std::string& line : s.status) {
        const ImVec2 t = ImGui::CalcTextSize(line.c_str());
        dl->AddRectFilled(ImVec2(map.view.x + 4, y - 2), ImVec2(map.view.x + 12 + t.x, y + t.y + 2),
                          kStrip);
        dl->AddText(ImVec2(map.view.x + 8, y), kText, line.c_str());
        y += t.y + 6;
    }
}

void VideoView::release() {
    main_.release();
    corner_.release();
    tiles_.clear();
}

}  // namespace gui
