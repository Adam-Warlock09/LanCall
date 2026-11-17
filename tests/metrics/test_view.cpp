// Unit tests for call/view.hpp (the lancall window image). No window is opened.

#include "call/view.hpp"

#include <opencv2/core.hpp>

#include "check.hpp"

using call::compose_view;
using call::inset_rect;
using call::kViewHeight;
using call::kViewWidth;

namespace {

const cv::Vec3b kRed(0, 0, 255);  // BGR
const cv::Vec3b kBlue(255, 0, 0);

cv::Mat solid(int w, int h, const cv::Vec3b& c) {
    return cv::Mat(h, w, CV_8UC3, cv::Scalar(c[0], c[1], c[2]));
}

// A pixel well away from the status text and the inset.
cv::Point remote_probe() {
    return {kViewWidth / 4, kViewHeight / 2};
}

void test_size_and_type() {
    const cv::Mat v = compose_view(solid(640, 480, kRed), cv::Mat(), {});
    CHECK(v.cols == kViewWidth && v.rows == kViewHeight);
    CHECK(v.type() == CV_8UC3);
}

void test_remote_fills_the_view() {
    const cv::Mat v = compose_view(solid(640, 480, kRed), cv::Mat(), {});
    CHECK(v.at<cv::Vec3b>(remote_probe()) == kRed);
    CHECK(v.at<cv::Vec3b>(kViewHeight - 1, kViewWidth - 1) == kRed);  // no inset without self
}

void test_inset_position_and_content() {
    const cv::Mat v = compose_view(solid(640, 480, kRed), solid(640, 480, kBlue), {});
    const cv::Rect r = inset_rect();
    CHECK(r.width == call::kInsetWidth && r.height == call::kInsetHeight);
    CHECK(r.x + r.width + call::kInsetMargin == kViewWidth);
    CHECK(r.y + r.height + call::kInsetMargin == kViewHeight);
    CHECK(v.at<cv::Vec3b>(r.y + r.height / 2, r.x + r.width / 2) == kBlue);  // inside the inset
    CHECK(v.at<cv::Vec3b>(r.y, r.x) == kBlue);
    CHECK(v.at<cv::Vec3b>(r.y + r.height - 1, r.x + r.width - 1) == kBlue);
    CHECK(v.at<cv::Vec3b>(remote_probe()) == kRed);                        // outside the inset
    CHECK(v.at<cv::Vec3b>(r.y - 1, r.x - 1) == cv::Vec3b(255, 255, 255));  // the frame
}

void test_waiting_card_without_remote() {
    const cv::Mat v = compose_view(cv::Mat(), cv::Mat(), {});
    CHECK(v.cols == kViewWidth && v.rows == kViewHeight);
    const cv::Vec3b corner = v.at<cv::Vec3b>(kViewHeight - 1, 0);
    CHECK(corner == cv::Vec3b(40, 40, 40));
    // The message is drawn in the middle: some pixel on the centre line is not the card colour.
    bool text = false;
    for (int x = 0; x < kViewWidth && !text; ++x) {
        text = v.at<cv::Vec3b>(kViewHeight / 2 - 5, x) != corner;
    }
    CHECK(text);
}

void test_other_sizes_and_grey_are_scaled() {
    const cv::Mat small = compose_view(solid(320, 240, kRed), solid(1280, 720, kBlue), {});
    CHECK(small.cols == kViewWidth && small.rows == kViewHeight);
    CHECK(small.at<cv::Vec3b>(remote_probe()) == kRed);
    const cv::Rect r = inset_rect();
    CHECK(small.at<cv::Vec3b>(r.y + 5, r.x + 5) == kBlue);

    const cv::Mat grey(240, 320, CV_8UC1, cv::Scalar(100));
    const cv::Mat g = compose_view(grey, cv::Mat(), {});
    CHECK(g.type() == CV_8UC3);
    CHECK(g.at<cv::Vec3b>(remote_probe()) == cv::Vec3b(100, 100, 100));
}

void test_status_text_drawn_and_input_untouched() {
    const cv::Mat remote = solid(640, 480, kRed);
    const cv::Mat v = compose_view(remote, cv::Mat(), {"Atul - in call with 10.8.66.20"});
    bool text = false;
    for (int x = 10; x < 300 && !text; ++x) {
        for (int y = 10; y < 30 && !text; ++y) text = v.at<cv::Vec3b>(y, x) != kRed;
    }
    CHECK(text);                                  // something was drawn top left
    CHECK(remote.at<cv::Vec3b>(20, 20) == kRed);  // the caller's frame was not drawn on
}

// ---- phase 4: the group-call grid --------------------------------------------------------

const cv::Vec3b kGreen(0, 255, 0);

void test_grid_rects() {
    CHECK(call::grid_rects(0).empty());
    CHECK(call::grid_rects(1).size() == 1 && call::grid_rects(1)[0] == cv::Rect(0, 0, 640, 480));
    const cv::Rect view(0, 0, kViewWidth, kViewHeight);
    for (int n = 1; n <= call::kMaxTiles; ++n) {
        const auto r = call::grid_rects(n);
        CHECK(static_cast<int>(r.size()) == n);
        for (size_t i = 0; i < r.size(); ++i) {
            // inside the view, 4:3 (213 x 159 is as close as whole pixels get), no overlap
            CHECK((r[i] & view) == r[i]);
            CHECK(r[i].width * 3 == r[i].height * 4 || n == 1 || r[i].width == 213);
            for (size_t j = 0; j < i; ++j) CHECK((r[i] & r[j]).area() == 0);
        }
    }
    CHECK(call::grid_rects(9).size() == static_cast<size_t>(call::kMaxTiles));  // clamped
    // 2 people side by side, centred vertically
    const auto two = call::grid_rects(2);
    CHECK(two[0] == cv::Rect(0, 120, 320, 240));
    CHECK(two[1] == cv::Rect(320, 120, 320, 240));
    // 3 people: 2 on top, the third centred below
    const auto three = call::grid_rects(3);
    CHECK(three[2] == cv::Rect(160, 240, 320, 240));
    // 6 people: 3 x 2 of 213 x 159
    const auto six = call::grid_rects(6);
    CHECK(six[0].size() == cv::Size(213, 159));
    CHECK(six[5].x == 426 && six[5].y == six[3].y);
}

void test_compose_grid() {
    std::vector<call::Tile> tiles = {{solid(640, 480, kRed), "Atul (you)"},
                                     {solid(320, 240, kBlue), "Aric"},
                                     {cv::Mat(), "Rishika"},  // no video yet
                                     {solid(640, 480, kGreen), "akshat"}};
    const cv::Mat v = call::compose_grid(tiles, {"Atul - group call, 4 people"});
    CHECK(v.cols == kViewWidth && v.rows == kViewHeight && v.type() == CV_8UC3);
    const auto r = call::grid_rects(4);
    auto centre = [&](int i) {
        return v.at<cv::Vec3b>(r[i].y + r[i].height / 2 + 30, r[i].x + r[i].width / 2 + 60);
    };
    CHECK(centre(0) == kRed);
    CHECK(centre(1) == kBlue);
    CHECK(centre(2) == cv::Vec3b(40, 40, 40));  // the "no video" card
    CHECK(centre(3) == kGreen);
    // the label is drawn bottom left of its tile
    bool label = false;
    for (int x = r[1].x + 8; x < r[1].x + 60 && !label; ++x)
        label = v.at<cv::Vec3b>(r[1].y + r[1].height - 14, x) != kBlue;
    CHECK(label);
}

void test_compose_grid_background() {
    // 2 tiles leave bands above and below the grid
    const cv::Mat v =
        call::compose_grid({{solid(64, 48, kRed), ""}, {solid(64, 48, kRed), ""}}, {});
    CHECK(v.at<cv::Vec3b>(kViewHeight - 5, kViewWidth / 2) == cv::Vec3b(15, 15, 15));
    CHECK(v.at<cv::Vec3b>(kViewHeight / 2, kViewWidth / 4) == kRed);
}

}  // namespace

int main() {
    RUN(test_size_and_type);
    RUN(test_remote_fills_the_view);
    RUN(test_inset_position_and_content);
    RUN(test_waiting_card_without_remote);
    RUN(test_other_sizes_and_grey_are_scaled);
    RUN(test_status_text_drawn_and_input_untouched);
    RUN(test_grid_rects);
    RUN(test_compose_grid);
    RUN(test_compose_grid_background);
    return check::result();
}
