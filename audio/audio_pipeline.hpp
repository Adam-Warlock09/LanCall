#pragma once
// Audio send and receive as reusable objects (Person 2, audio/). Used by the lancall app;
// audio_sender and audio_receiver become thin wrappers around these.
//
// FROZEN after the phase 2 kickoff: change only by agreement. Everything private lives
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
#include <cstdint>
#include <memory>
#include <string>

namespace audio {

struct SenderOptions {
    int in_device = -1;  // PortAudio input device, -1 = system default
    uint8_t stream_id = 0;
};

struct ReceiverOptions {
    int out_device = -1;  // PortAudio output device, -1 = system default
    int buffer_ms = 60;   // playout buffer before playback starts
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

    std::string stats() const;  // one line for the UI
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// UDP -> playout buffer -> speaker
class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();
    AudioReceiver(const AudioReceiver&) = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    // only_from_ip empty = accept the first sender that shows up
    bool start(uint16_t port, const std::string& only_from_ip = "",
               const ReceiverOptions& opt = {});
    void stop();
    bool running() const;

    // The audio clock for lip sync: sender capture time of the sound leaving the speaker
    // right now, i.e. the last chunk handed to PortAudio minus the output latency.
    // false before playback has started, or while only silence is being played.
    // Safe from any thread (the callback only publishes atomics).
    bool playing_capture_us(uint32_t& out) const;

    std::string stats() const;  // one line for the UI
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace audio