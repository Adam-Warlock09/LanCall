#include "call/view.hpp"

#include <opencv2/imgproc.hpp>

namespace call {

namespace {

const cv::Scalar kCardColor(40, 40, 40);  // BGR
const cv::Scalar kTextColor(255, 255, 255);
const cv::Scalar kShadowColor(0, 0, 0);
const cv::Scalar kFrameColor(255, 255, 255);

// Converts to 3-channel BGR and scales to `size` (no-op when it already matches).
cv::Mat to_bgr(const cv::Mat& in, const cv::Size& size) {
    cv::Mat bgr;
    if (in.channels() == 1) {
        cv::cvtColor(in, bgr, cv::COLOR_GRAY2BGR);
    } else if (in.channels() == 4) {
        cv::cvtColor(in, bgr, cv::COLOR_BGRA2BGR);
    } else {
        bgr = in;
    }
    if (bgr.size() == size) return bgr;
    cv::Mat out;
    cv::resize(bgr, out, size, 0, 0, cv::INTER_AREA);
    return out;
}

// White text with a dark outline, readable on any video.
void draw_text(cv::Mat& img, const std::string& text, cv::Point at, double scale) {
    cv::putText(img, text, at, cv::FONT_HERSHEY_SIMPLEX, scale, kShadowColor, 3, cv::LINE_AA);
    cv::putText(img, text, at, cv::FONT_HERSHEY_SIMPLEX, scale, kTextColor, 1, cv::LINE_AA);
}

}  // namespace

cv::Rect inset_rect() {
    return {kViewWidth - kInsetMargin - kInsetWidth, kViewHeight - kInsetMargin - kInsetHeight,
            kInsetWidth, kInsetHeight};
}

cv::Mat compose_view(const cv::Mat& remote, const cv::Mat& self,
                     const std::vector<std::string>& status_lines) {
    const cv::Size view_size(kViewWidth, kViewHeight);
    cv::Mat view;
    if (remote.empty()) {
        view = cv::Mat(view_size, CV_8UC3, kCardColor);
        const std::string msg = "waiting for video...";
        int baseline = 0;
        const cv::Size ts = cv::getTextSize(msg, cv::FONT_HERSHEY_SIMPLEX, 0.8, 1, &baseline);
        draw_text(view, msg, {(kViewWidth - ts.width) / 2, kViewHeight / 2}, 0.8);
    } else {
        view = to_bgr(remote, view_size).clone();  // never draw on the caller's frame
    }

    if (!self.empty()) {
        const cv::Rect r = inset_rect();
        to_bgr(self, r.size()).copyTo(view(r));
        cv::rectangle(view, {r.x - 1, r.y - 1, r.width + 2, r.height + 2}, kFrameColor, 1);
    }

    int y = 24;
    for (const std::string& line : status_lines) {
        draw_text(view, line, {10, y}, 0.55);
        y += 22;
    }
    return view;
}

}  // namespace call
