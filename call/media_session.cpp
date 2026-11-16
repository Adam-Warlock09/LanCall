#include "call/media_session.hpp"

#include "common/protocol.hpp"

namespace call {

MediaSession::~MediaSession() {
    stop();
}

std::vector<std::string> MediaSession::start(const std::string& peer_ip, const MediaOptions& opt) {
    stop();
    std::vector<std::string> failed;

    // Receivers first: their ports are bound before the peer's first packet arrives.
    arx_ = std::make_unique<audio::AudioReceiver>();
    audio::ReceiverOptions ao;
    ao.out_device = opt.out_device;
    ao.buffer_ms = opt.buffer_ms;
    if (!arx_->start(proto::kAudioPort, peer_ip, ao)) {
        failed.push_back("audio receive: " + arx_->error());
        arx_.reset();
    }

    vrx_ = std::make_unique<video::VideoReceiver>();
    if (!vrx_->start(proto::kVideoPort, peer_ip)) {
        failed.push_back("video receive: " + vrx_->error());
        vrx_.reset();
    }

    atx_ = std::make_unique<audio::AudioSender>();
    audio::SenderOptions as;
    as.in_device = opt.in_device;
    if (!atx_->start(peer_ip, proto::kAudioPort, as)) {
        failed.push_back("audio send: " + atx_->error());
        atx_.reset();
    }

    vtx_ = std::make_unique<video::VideoSender>();
    video::SenderOptions vs;
    vs.camera = opt.camera;
    vs.fps = opt.fps;
    vs.quality = opt.quality;
    if (!vtx_->start(peer_ip, proto::kVideoPort, vs)) {
        failed.push_back("video send: " + vtx_->error());
        vtx_.reset();
    }

    peer_ip_ = peer_ip;
    active_ = atx_ || arx_ || vtx_ || vrx_;
    return failed;
}

void MediaSession::stop() {
    if (!active_) return;
    summary_.clear();
    // Senders first, so the peer stops hearing and seeing us right away.
    if (vtx_) {
        vtx_->stop();
        summary_.push_back(vtx_->stats());
    }
    if (atx_) {
        atx_->stop();
        summary_.push_back(atx_->stats());
    }
    if (vrx_) {
        vrx_->stop();
        summary_.push_back(vrx_->stats());
    }
    if (arx_) {
        arx_->stop();
        summary_.push_back(arx_->stats());
    }
    vtx_.reset();
    atx_.reset();
    vrx_.reset();
    arx_.reset();
    active_ = false;
}

std::string MediaSession::parts() const {
    const bool audio = atx_ || arx_;
    const bool video = vtx_ || vrx_;
    if (audio && video) return "audio+video";
    if (audio) return "audio only";
    if (video) return "video only";
    return "nothing";
}

bool MediaSession::audio_clock(uint32_t& out) const {
    return arx_ && arx_->playing_capture_us(out);
}

bool MediaSession::pop_frame(video::VideoFrame& out) {
    return vrx_ && vrx_->pop(out);
}

bool MediaSession::self_view(cv::Mat& out) const {
    return vtx_ && vtx_->self_view(out);
}

std::vector<std::string> MediaSession::stats() const {
    std::vector<std::string> lines;
    if (atx_) lines.push_back(atx_->stats());
    if (arx_) lines.push_back(arx_->stats());
    if (vtx_) lines.push_back(vtx_->stats());
    if (vrx_) lines.push_back(vrx_->stats());
    return lines;
}

}  // namespace call
