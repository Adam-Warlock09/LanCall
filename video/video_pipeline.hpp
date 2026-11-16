#pragma once
// Video send and receive as reusable objects (Person 1, video/). Used by the lancall app;
// video_sender and video_receiver become thin wrappers around these.
//
// FROZEN after the phase 2 kickoff: change only by agreement. Everything private lives
// behind Impl, so the implementation can change freely without touching this file.
//
//   video::VideoSender tx;
//   tx.start("10.8.66.20", proto::kVideoPort);      // camera + encode + send threads
//   cv::Mat me; tx.self_view(me);                   // latest local frame, for a thumbnail
//
//   video::VideoReceiver rx;
//   rx.start(proto::kVideoPort, "10.8.66.20");      // only accept that sender
//   video::VideoFrame f;
//   while (rx.pop(f)) { ...decide when to show f.image, using f.capture_us... }
//
// Neither class ever calls imshow/waitKey: OpenCV windows belong to the app's main thread.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

namespace video {

struct VideoFrame {
    cv::Mat image;            // BGR, owns its pixels, safe to keep after pop()
    uint32_t capture_us = 0;  // sender's capture time, same clock as the sender's audio
};

struct SenderOptions {
    int fps = 15;
    int quality = 70;  // JPEG quality, 1-100
    int strips = 10;   // 1 = whole frame
    int width = 640;
    int height = 480;
    int camera = 0;
    uint8_t stream_id = 0;
};

// camera -> strips -> UDP, on its own threads
class VideoSender {
public:
    VideoSender();
    ~VideoSender();  // stops if still running
    VideoSender(const VideoSender&) = delete;
    VideoSender& operator=(const VideoSender&) = delete;

    // false + error() if the camera or socket can't be opened
    bool start(const std::string& dest_ip, uint16_t port, const SenderOptions& opt = {});
    void stop();
    bool running() const;

    // newest local camera frame (a copy). false until the camera has produced one. any thread
    bool self_view(cv::Mat& out) const;

    // one line for the UI, e.g. "tx 15 fps 4.6 Mbps"
    std::string stats() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// UDP -> strips -> canvas, on its own thread. Every time the canvas is complete for one
// capture time, a copy goes into a small queue for the app to pop.
class VideoReceiver {
public:
    static constexpr size_t kMaxQueued = 30;  // ~2 s at 15 fps; oldest dropped when full

    VideoReceiver();
    ~VideoReceiver();
    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // only_from_ip empty = accept any sender. false + error() if the port can't be bound
    bool start(uint16_t port, const std::string& only_from_ip = "");
    void stop();
    bool running() const;

    // oldest queued frame, in capture order. false if the queue is empty. any thread
    bool pop(VideoFrame& out);
    size_t queued() const;

    // one line for the UI, e.g. "rx 15 fps 4.5 Mbps strips 2% missing"
    std::string stats() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace video