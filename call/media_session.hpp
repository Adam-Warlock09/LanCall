#pragma once
// MediaSession — the four media objects of one call (Person 2, call/).
//
// start() creates fresh AudioSender/AudioReceiver/VideoSender/VideoReceiver objects for the
// peer, receivers first (so their ports are bound before the peer's first packet), and
// accepts media only from the peer's IP. A part that fails to start does not end the call:
// start() returns what failed and the call carries on with the rest (for example audio only
// while the video pipeline is unavailable). stop() stops senders first, then receivers, and
// keeps each part's final stats line for summary().
//
// Use from one thread (the app's main loop).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "audio/audio_pipeline.hpp"
#include "video/video_pipeline.hpp"

namespace call {

struct MediaOptions {
    int in_device = -1;   // PortAudio input, -1 = default
    int out_device = -1;  // PortAudio output, -1 = default
    int buffer_ms = 60;   // audio playout buffer
    int camera = 0;
    int fps = 15;
    int quality = 70;
};

class MediaSession {
public:
    MediaSession() = default;
    ~MediaSession();
    MediaSession(const MediaSession&) = delete;
    MediaSession& operator=(const MediaSession&) = delete;

    // Starts media with peer_ip. Returns one message per part that failed (empty = all four
    // started). Ends any previous session first.
    std::vector<std::string> start(const std::string& peer_ip, const MediaOptions& opt);
    void stop();

    bool active() const { return active_; }
    const std::string& peer_ip() const { return peer_ip_; }
    std::string parts() const;  // e.g. "audio+video", "audio only", "nothing"

    // Audio clock for lip sync (sender capture time of the audio being heard now).
    bool audio_clock(uint32_t& out) const;
    // Next received video frame, oldest first.
    bool pop_frame(video::VideoFrame& out);
    // Newest local camera frame for the self-view.
    bool self_view(cv::Mat& out) const;

    // One stats line per running part.
    std::vector<std::string> stats() const;
    // Final stats lines of the last stopped session.
    const std::vector<std::string>& summary() const { return summary_; }

private:
    std::unique_ptr<audio::AudioSender> atx_;
    std::unique_ptr<audio::AudioReceiver> arx_;
    std::unique_ptr<video::VideoSender> vtx_;
    std::unique_ptr<video::VideoReceiver> vrx_;
    bool active_ = false;
    std::string peer_ip_;
    std::vector<std::string> summary_;
};

}  // namespace call
