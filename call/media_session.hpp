#pragma once
// MediaSession — the four media objects of one call (Person 2, call/).
//
// start() creates fresh AudioSender/AudioReceiver/VideoSender/VideoReceiver objects for a
// Route (call/call_logic.hpp), receivers first (so their ports are bound before the first
// packet), and accepts media only from the route's sources. A part that fails to start does
// not end the call: start() returns what failed and the call carries on with the rest (for
// example audio only while the video pipeline is unavailable). stop() stops senders first,
// then receivers, and keeps each part's final stats line for summary().
//
// Phase 4: in a group call the route changes while media runs (someone joined or left, mesh
// <-> relay); update_route() re-points the senders and the receivers' source filters without
// restarting anything. Everything we send carries our member id as stream_id, and the audio
// clock is per stream, since every sender has its own capture clock.
//
// Use from one thread (the app's main loop).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "audio/audio_pipeline.hpp"
#include "call/call_logic.hpp"
#include "video/video_pipeline.hpp"

namespace call {

struct MediaOptions {
    int in_device = -1;   // PortAudio input, -1 = default
    int out_device = -1;  // PortAudio output, -1 = default
    int buffer_ms = 60;   // audio playout buffer
    int camera = 0;
    int fps = 15;
    int quality = 70;
    bool fec = false;  // phase 5: a parity packet per video strip (NACK is always on)
};

class MediaSession {
public:
    MediaSession() = default;
    ~MediaSession();
    MediaSession(const MediaSession&) = delete;
    MediaSession& operator=(const MediaSession&) = delete;

    // Starts media along `route`, sending with stream_id = my_id (our member id, 0 outside
    // group calls). Returns one message per part that failed (empty = all four started).
    // Ends any previous session first.
    std::vector<std::string> start(const Route& route, const MediaOptions& opt, uint8_t my_id = 0);
    void stop();

    // Phase 4: new destinations and sources for the running parts.
    void update_route(const Route& route);

    bool active() const { return active_; }
    std::string parts() const;  // e.g. "audio+video", "audio only", "nothing"

    // Audio clock for lip sync (sender capture time of the audio being heard now).
    bool audio_clock(uint32_t& out) const;
    // Phase 4: the same for one sender's stream.
    bool audio_clock(uint8_t stream_id, uint32_t& out) const;
    // Phase 4: streams heard in the last 2 s, ascending.
    std::vector<uint8_t> audio_streams() const;
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
    std::vector<std::string> summary_;
};

}  // namespace call
