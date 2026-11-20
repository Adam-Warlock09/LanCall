#pragma once
// Audio send and receive as reusable objects (Person 2, audio/). Used by the lancall app;
// audio_sender and audio_receiver become thin wrappers around these.
//
// FROZEN after the phase 4 kickoff: change only by agreement. Everything private lives
// behind Impl, so the implementation can change freely without touching this file.
//
//   audio::AudioSender tx;
//   tx.start("10.8.66.20", proto::kAudioPort);      // mic -> UDP
//
//   audio::AudioReceiver rx;
//   rx.start(proto::kAudioPort, "10.8.66.20");      // UDP -> playout buffer -> speaker
//   uint32_t t;
//   if (rx.playing_capture_us(t)) { ...show the video frame captured closest to t... }
//
// Each object owns whatever PortAudio setup it needs (Pa_Initialize is reference counted,
// so a sender and a receiver in one process can each initialise it).
//
// Phase 4, group calls: the sender can send to several places at once, and the receiver
// keeps one playout buffer per stream_id (= the sender's member id) and mixes them in the
// output callback: add the samples of every stream that has a chunk ready, clip to int16.
// Lip sync then needs the audio clock of each stream separately, since every sender has
// its own capture clock: playing_capture_us(stream_id, t).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/protocol.hpp"

namespace audio {

struct SenderOptions {
    int in_device = -1;        // PortAudio input device, -1 = system default
    uint8_t stream_id = 0;     // our member id in a group call (Signaling::my_id())
    std::string multicast_if;  // phase 7: our wired address, for sending to groups. "" = any
};

struct ReceiverOptions {
    int out_device = -1;  // PortAudio output device, -1 = system default
    int buffer_ms = 60;   // playout buffer before playback starts
    uint8_t my_id = 0;    // phase 4: goes into our receiver reports, 0 outside group calls
};

// microphone -> UDP
class AudioSender {
public:
    AudioSender();
    ~AudioSender();  // stops if still running
    AudioSender(const AudioSender&) = delete;
    AudioSender& operator=(const AudioSender&) = delete;

    bool start(const std::string& dest_ip, uint16_t port, const SenderOptions& opt = {});
    void stop();
    bool running() const;

    // Phase 4: replace the list of places every chunk goes to (start() sets a list of one).
    // An empty list = muted on the network. any thread
    void set_destinations(const std::vector<proto::Endpoint>& to);

    std::string stats() const;  // one line for the UI
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// UDP -> one playout buffer per stream -> mix -> speaker
class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();
    AudioReceiver(const AudioReceiver&) = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    // only_from_ip empty = accept anyone (phase 4; it used to be "the first sender")
    bool start(uint16_t port, const std::string& only_from_ip = "",
               const ReceiverOptions& opt = {});
    void stop();
    bool running() const;

    // Phase 4: accept packets from any of these ips (empty = anyone). Replaces only_from_ip.
    // In relay mode that's just the relay. any thread
    void set_allowed_sources(const std::vector<std::string>& ips);

    // The audio clock for lip sync: sender capture time of the sound leaving the speaker
    // right now, i.e. the last chunk handed to PortAudio minus the output latency.
    // false before playback has started, or while only silence is being played.
    // Safe from any thread (the callback only publishes atomics).
    bool playing_capture_us(uint32_t& out) const;

    // Phase 4: the same for one stream. false if that stream isn't playing right now
    // (not started, or silent), so its video should just be shown as it arrives.
    bool playing_capture_us(uint8_t stream_id, uint32_t& out) const;

    // Phase 4: stream ids that played sound in the last 2 s, sorted
    std::vector<uint8_t> active_streams() const;

    // Phase 7: also receive what's sent to this multicast group (on our port), arriving on
    // the card with address iface_ip ("" = let the kernel pick). One group at a time:
    // joining another leaves the first. false + error() if it can't be joined.
    // set_allowed_sources() still applies. Left automatically on stop(). any thread
    bool join_group(const std::string& group, const std::string& iface_ip = "");
    void leave_group();

    std::string stats() const;  // one line for the UI
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace audio