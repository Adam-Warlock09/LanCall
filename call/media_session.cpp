#include "call/media_session.hpp"

#include "common/protocol.hpp"

namespace call {

MediaSession::~MediaSession() {
    stop();
}

namespace {

// Senders start with one destination; an empty list starts on a placeholder that is muted at
// once (it can only reach our own receivers, which filter it out by source).
proto::Endpoint first_or_placeholder(const std::vector<proto::Endpoint>& to, uint16_t port) {
    return to.empty() ? proto::Endpoint{"127.0.0.1", port} : to.front();
}

}  // namespace

std::vector<std::string> MediaSession::start(const Route& route, const MediaOptions& opt,
                                             uint8_t my_id) {
    stop();
    std::vector<std::string> failed;
    // one source: the start() filter is enough; several (or anyone): set the list after start
    const std::string only_from = route.sources.size() == 1 ? route.sources.front() : "";

    // Receivers first: their ports are bound before the first packet arrives.
    arx_ = std::make_unique<audio::AudioReceiver>();
    audio::ReceiverOptions ao;
    ao.out_device = opt.out_device;
    ao.buffer_ms = opt.buffer_ms;
    ao.my_id = my_id;
    if (!arx_->start(proto::kAudioPort, only_from, ao)) {
        failed.push_back("audio receive: " + arx_->error());
        arx_.reset();
    } else if (route.sources.size() > 1) {
        arx_->set_allowed_sources(route.sources);
    }

    vrx_ = std::make_unique<video::VideoReceiver>();
    if (!vrx_->start(proto::kVideoPort, only_from, my_id)) {
        failed.push_back("video receive: " + vrx_->error());
        vrx_.reset();
    } else if (route.sources.size() > 1) {
        vrx_->set_allowed_sources(route.sources);
    }

    atx_ = std::make_unique<audio::AudioSender>();
    audio::SenderOptions as;
    as.in_device = opt.in_device;
    as.stream_id = my_id;
    const proto::Endpoint a0 = first_or_placeholder(route.audio_to, proto::kAudioPort);
    if (!atx_->start(a0.ip, a0.port, as)) {
        failed.push_back("audio send: " + atx_->error());
        atx_.reset();
    } else if (route.audio_to.size() != 1) {
        atx_->set_destinations(route.audio_to);
    }

    vtx_ = std::make_unique<video::VideoSender>();
    video::SenderOptions vs;
    vs.camera = opt.camera;
    vs.fps = opt.fps;
    vs.quality = opt.quality;
    vs.fec = opt.fec;
    vs.stream_id = my_id;
    const proto::Endpoint v0 = first_or_placeholder(route.video_to, proto::kVideoPort);
    if (!vtx_->start(v0.ip, v0.port, vs)) {
        failed.push_back("video send: " + vtx_->error());
        vtx_.reset();
    } else if (route.video_to.size() != 1) {
        vtx_->set_destinations(route.video_to);
    }

    active_ = atx_ || arx_ || vtx_ || vrx_;
    return failed;
}

void MediaSession::update_route(const Route& route) {
    if (atx_) atx_->set_destinations(route.audio_to);
    if (vtx_) vtx_->set_destinations(route.video_to);
    if (arx_) arx_->set_allowed_sources(route.sources);
    if (vrx_) vrx_->set_allowed_sources(route.sources);
}

void MediaSession::set_crypto(std::shared_ptr<const proto::MediaCrypto> crypto) {
    if (atx_) atx_->set_crypto(crypto);
    if (arx_) arx_->set_crypto(crypto);
    if (vtx_) vtx_->set_crypto(crypto);
    if (vrx_) vrx_->set_crypto(crypto);
    crypto_ = std::move(crypto);
}

void MediaSession::stop() {
    if (!active_) return;
    crypto_.reset();
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

bool MediaSession::audio_clock(uint8_t stream_id, uint32_t& out) const {
    return arx_ && arx_->playing_capture_us(stream_id, out);
}

std::vector<uint8_t> MediaSession::audio_streams() const {
    return arx_ ? arx_->active_streams() : std::vector<uint8_t>{};
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
