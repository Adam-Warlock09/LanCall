#pragma once
// Video send and receive as reusable objects (Person 1, video/). Used by the lancall app;
// video_sender and video_receiver become thin wrappers around these.
//
// FROZEN after the phase 5 kickoff: change only by agreement. Everything private lives
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
//
// Phase 3: the receiver sends receiver reports (common/report.hpp) back to the sender every
// 500 ms. With adaptive on, the sender steps quality and fps down when loss climbs and back
// up when it stays low; fps and quality in SenderOptions are then the top step.
//
// Phase 4, group calls: the sender can send to several places at once (every other member in
// mesh mode, or just the relay), and the receiver takes several streams at once, told apart
// by stream_id (= the sender's member id), each with its own canvas:
//
//   tx.set_destinations(sig.media_destinations(proto::kVideoPort));   // after every roster change
//   rx.set_allowed_sources({...member ips, or the relay ip...});
//   while (rx.pop(f)) tiles[f.stream_id] = f;
//
// With several receivers the ladder follows the worst one, so one bad link drags everyone's
// view of us down. That's the price of sending a single encoding; E5 measures it.
//
// Phase 5: receivers NACK lost packets and senders resend them (common/nack.hpp), so a frame
// that lost a packet can still be shown whole. A frame missing strips waits up to 20 ms for
// the repairs. SenderOptions::nack off ignores NACKs (for comparing in experiment E8).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "common/protocol.hpp"

namespace video {

struct VideoFrame {
    cv::Mat image;            // BGR, owns its pixels, safe to keep after pop()
    uint32_t capture_us = 0;  // sender's capture time, same clock as the sender's audio
    uint8_t stream_id = 0;    // who sent it (their member id in a group call)
};

struct SenderOptions {
    int fps = 15;
    int quality = 70;  // JPEG quality, 1-100
    int strips = 10;   // 1 = whole frame
    int width = 640;
    int height = 480;
    int camera = 0;
    uint8_t stream_id = 0;  // our member id in a group call (Signaling::my_id())
    bool adaptive = true;   // phase 3: react to receiver reports. off = fixed fps and quality
    bool nack = true;       // phase 5: resend packets receivers ask for again
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

    // Phase 4: replace the list of places every strip goes to (start() sets a list of one).
    // An empty list pauses sending; the camera keeps running for the self view. Reports from
    // receivers that are no longer listed are ignored. any thread
    void set_destinations(const std::vector<proto::Endpoint>& to);

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
// capture time, a copy goes into a small queue for the app to pop. Each stream_id gets its
// own canvas, and its own receiver report sent back to wherever that stream came from.
class VideoReceiver {
public:
    static constexpr size_t kMaxQueued = 30;  // per stream, ~2 s at 15 fps; oldest dropped

    VideoReceiver();
    ~VideoReceiver();
    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // only_from_ip empty = accept any sender. my_id goes into our receiver reports (phase 4,
    // Signaling::my_id(), 0 outside group calls). false + error() if the port can't be bound
    bool start(uint16_t port, const std::string& only_from_ip = "", uint8_t my_id = 0);
    void stop();
    bool running() const;

    // Phase 4: accept packets from any of these ips (empty = anyone). Replaces only_from_ip.
    // In relay mode that's just the relay. any thread
    void set_allowed_sources(const std::vector<std::string>& ips);

    // Phase 4: stream ids that delivered a frame in the last 2 s, sorted. For the grid
    std::vector<uint8_t> active_streams() const;

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