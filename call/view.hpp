#pragma once
// The lancall window image (Person 2, call/): the other person's video, a small self-view and
// a few lines of status text. Pure drawing on cv::Mat, no window calls, so it is unit-tested
// on its own (tests/metrics/test_view.cpp); main.cpp shows the result with cv::imshow.
//
//   +------------------------------------------+
//   | Atul - in call with 10.8.66.20           |   status lines, top left
//   |                                          |
//   |        remote video, 640 x 480           |   (or a "waiting for video" card)
//   |                                          |
//   |                            +----------+  |
//   |                            | self 160 |  |   self-view inset, bottom right
//   |                            +----------+  |
//   +------------------------------------------+

#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace call {

constexpr int kViewWidth = 640;
constexpr int kViewHeight = 480;
constexpr int kInsetWidth = 160;
constexpr int kInsetHeight = 120;
constexpr int kInsetMargin = 12;

// Where the self-view goes in the composed image (inside a 1-pixel frame).
cv::Rect inset_rect();

// remote: last frame shown for the other person (any size, BGR or grey), or empty for the
//         "waiting for video" card. self: newest local camera frame, or empty for no inset.
// Returns a kViewWidth x kViewHeight BGR image.
cv::Mat compose_view(const cv::Mat& remote, const cv::Mat& self,
                     const std::vector<std::string>& status_lines);

}  // namespace call
