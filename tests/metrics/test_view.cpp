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

}  // namespace

int main() {
    RUN(test_size_and_type);
    RUN(test_remote_fills_the_view);
    RUN(test_inset_position_and_content);
    RUN(test_waiting_card_without_remote);
    RUN(test_other_sizes_and_grey_are_scaled);
    RUN(test_status_text_drawn_and_input_untouched);
    return check::result();
}
