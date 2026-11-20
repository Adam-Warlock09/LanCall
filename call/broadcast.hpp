#pragma once
// Broadcasts (Person 2, call/, phase 7): one presenter, any number of viewers on the floor.
//
// The presenter sends its audio and video ONCE, to a multicast group (proto::lancall_group,
// TTL 1, so it stays on the floor) on the normal media ports 5006 / 5004, and announces it in
// its discovery beacon (mc= and title=, control/discovery.hpp). A viewer joins that group and
// accepts packets from the presenter's address only. There's no call setup, and viewers
// send no media, so a broadcast costs the presenter one upload copy however many watch.
//
// Choices made for broadcasts (see NOTES.md, phase 7):
//   - video at a fixed level: there is one encoding for everyone, and following the worst of
//     many viewers would drag the whole room down
//   - FEC on and NACK off at the presenter; viewers don't NACK either (set_nack(false)) and
//     show a frame with a missing strip at once. Parity repairs a lost packet for every
//     viewer at once, without any traffic back to the presenter
//   - not encrypted (v1): anyone on the floor may watch, that's the point of it
//   - the group is flooded to every port on the floor (no IGMP snooping), watching or not
//
// Use both from one thread (the app's main loop). What they hold is there for a UI to read,
// not only for printing.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "audio/audio_pipeline.hpp"
#include "video/video_pipeline.hpp"

namespace call {

struct BroadcastOptions {
    int in_device = -1;  // PortAudio input, -1 = default
    int camera = 0;
    int fps = 15;  // the fixed video level
    int quality = 70;
    std::string iface_ip;  // our wired address (Discovery::own_ip()); "" = the kernel picks
    bool loop = false;     // hear our own group packets: only for one-laptop tests
};

// The presenter's side: microphone and camera -> the group.
class BroadcastSession {
public:
    static constexpr uint8_t kStreamId = 1;  // the stream id of both streams

    BroadcastSession() = default;
    ~BroadcastSession();
    BroadcastSession(const BroadcastSession&) = delete;
    BroadcastSession& operator=(const BroadcastSession&) = delete;

    // group: 1..254 (Discovery::pick_group()). Returns one message per part that failed;
    // the broadcast is on if either part started (see active()). Ends any previous one.
    std::vector<std::string> start(const std::string& title, uint8_t group,
                                   const BroadcastOptions& opt);
    void stop();

    bool active() const { return active_; }
    const std::string& title() const { return title_; }
    void set_title(const std::string& title) { title_ = title; }  // the media goes on as it is
    uint8_t group() const { return group_; }
    std::string group_ip() const;
    uint32_t started_us() const { return started_us_; }  // proto::now_us() at start()
    std::string parts() const;                           // "audio+video", "audio only", ...

    bool self_view(cv::Mat& out) const;  // our camera, for the presenter's own window
    std::vector<std::string> stats() const;
    const std::vector<std::string>& summary() const { return summary_; }  // after stop()

private:
    std::unique_ptr<audio::AudioSender> atx_;
    std::unique_ptr<video::VideoSender> vtx_;
    bool active_ = false;
    std::string title_;
    uint8_t group_ = 0;
    uint32_t started_us_ = 0;
    std::vector<std::string> summary_;
};

struct WatchOptions {
    int out_device = -1;   // PortAudio output, -1 = default
    int buffer_ms = 60;    // audio playout buffer
    std::string iface_ip;  // our wired address, to join the group on; "" = the kernel picks
};

// A viewer's side: the group -> speaker and window. Offers the same calls as MediaSession for
// received media (pop_frame, audio_clock, audio_streams), so the app's lip sync serves both.
class WatchSession {
public:
    WatchSession() = default;
    ~WatchSession();
    WatchSession(const WatchSession&) = delete;
    WatchSession& operator=(const WatchSession&) = delete;

    // name / title: who and what, for the status line. Returns one message per part that
    // failed; watching is on if either part started. Ends any previous one.
    std::vector<std::string> start(const std::string& name, const std::string& title,
                                   const std::string& presenter_ip, uint8_t group,
                                   const WatchOptions& opt);
    void stop();
    // the presenter moved to another group (rare): leave the old one, join this
    bool move_to(uint8_t group);

    bool active() const { return active_; }
    const std::string& name() const { return name_; }
    const std::string& title() const { return title_; }
    const std::string& presenter_ip() const { return presenter_ip_; }
    uint8_t group() const { return group_; }
    std::string parts() const;

    bool audio_clock(uint32_t& out) const;
    bool audio_clock(uint8_t stream_id, uint32_t& out) const;
    std::vector<uint8_t> audio_streams() const;
    bool pop_frame(video::VideoFrame& out);

    std::vector<std::string> stats() const;
    const std::vector<std::string>& summary() const { return summary_; }  // after stop()

private:
    std::unique_ptr<audio::AudioReceiver> arx_;
    std::unique_ptr<video::VideoReceiver> vrx_;
    bool active_ = false;
    std::string name_, title_, presenter_ip_, iface_ip_;
    uint8_t group_ = 0;
    std::vector<std::string> summary_;
};

}  // namespace call
