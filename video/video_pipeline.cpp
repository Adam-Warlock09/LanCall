// PLACEHOLDER from the phase 2 kickoff, so call/ links from day one.
// Person 1 replaces this with the real pipelines (moving the logic out of
// video_sender.cpp / video_receiver.cpp).
#include "video/video_pipeline.hpp"

namespace video {

struct VideoSender::Impl {};

VideoSender::VideoSender() : impl_(std::make_unique<Impl>()) {}
VideoSender::~VideoSender() = default;
bool VideoSender::start(const std::string&, uint16_t, const SenderOptions&) {
    return false;
}
void VideoSender::stop() {}
bool VideoSender::running() const {
    return false;
}
bool VideoSender::self_view(cv::Mat&) const {
    return false;
}
std::string VideoSender::stats() const {
    return "video tx not implemented";
}
std::string VideoSender::error() const {
    return "video tx not implemented yet";
}

struct VideoReceiver::Impl {};

VideoReceiver::VideoReceiver() : impl_(std::make_unique<Impl>()) {}
VideoReceiver::~VideoReceiver() = default;
bool VideoReceiver::start(uint16_t, const std::string&) {
    return false;
}
void VideoReceiver::stop() {}
bool VideoReceiver::running() const {
    return false;
}
bool VideoReceiver::pop(VideoFrame&) {
    return false;
}
size_t VideoReceiver::queued() const {
    return 0;
}
std::string VideoReceiver::stats() const {
    return "video rx not implemented";
}
std::string VideoReceiver::error() const {
    return "video rx not implemented yet";
}

}  // namespace video