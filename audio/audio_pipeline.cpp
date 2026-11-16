// PLACEHOLDER from the phase 2 kickoff, so call/ links from day one.
// Person 2 replaces this with the real pipelines (moving the logic out of
// audio_sender.cpp / audio_receiver.cpp).
#include "audio/audio_pipeline.hpp"

namespace audio {

struct AudioSender::Impl {};

AudioSender::AudioSender() : impl_(std::make_unique<Impl>()) {}
AudioSender::~AudioSender() = default;
bool AudioSender::start(const std::string&, uint16_t, const SenderOptions&) {
    return false;
}
void AudioSender::stop() {}
bool AudioSender::running() const {
    return false;
}
std::string AudioSender::stats() const {
    return "audio tx not implemented";
}
std::string AudioSender::error() const {
    return "audio tx not implemented yet";
}

struct AudioReceiver::Impl {};

AudioReceiver::AudioReceiver() : impl_(std::make_unique<Impl>()) {}
AudioReceiver::~AudioReceiver() = default;
bool AudioReceiver::start(uint16_t, const std::string&, const ReceiverOptions&) {
    return false;
}
void AudioReceiver::stop() {}
bool AudioReceiver::running() const {
    return false;
}
bool AudioReceiver::playing_capture_us(uint32_t&) const {
    return false;
}
std::string AudioReceiver::stats() const {
    return "audio rx not implemented";
}
std::string AudioReceiver::error() const {
    return "audio rx not implemented yet";
}

}  // namespace audio