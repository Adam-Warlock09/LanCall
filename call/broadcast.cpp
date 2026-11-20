#include "call/broadcast.hpp"

#include <arpa/inet.h>

#include <algorithm>

#include "common/protocol.hpp"
#include "net/bridge.hpp"

namespace call {

namespace {

std::string parts_of(bool audio, bool video) {
    if (audio && video) return "audio+video";
    if (audio) return "audio only";
    if (video) return "video only";
    return "nothing";
}

}  // namespace

// ---- presenter -----------------------------------------------------------------------------

BroadcastSession::~BroadcastSession() {
    stop();
}

std::vector<std::string> BroadcastSession::start(const std::string& title, uint8_t group,
                                                 const BroadcastOptions& opt) {
    stop();
    std::vector<std::string> failed;
    const std::string to = proto::lancall_group(group);
    if (to.empty()) return {"no multicast group to send to"};

    atx_ = std::make_unique<audio::AudioSender>();
    audio::SenderOptions as;
    as.in_device = opt.in_device;
    as.stream_id = kStreamId;
    as.multicast_if = opt.iface_ip;
    as.multicast_loop = opt.loop;
    if (!atx_->start(to, proto::kAudioPort, as)) {
        failed.push_back("audio send: " + atx_->error());
        atx_.reset();
    }

    vtx_ = std::make_unique<video::VideoSender>();
    video::SenderOptions vs;
    vs.camera = opt.camera;
    vs.fps = opt.fps;
    vs.quality = opt.quality;
    vs.stream_id = kStreamId;
    vs.adaptive = false;  // one fixed level for everyone (see the header)
    vs.nack = false;
    vs.fec = true;
    vs.multicast_if = opt.iface_ip;
    vs.multicast_loop = opt.loop;
    if (!vtx_->start(to, proto::kVideoPort, vs)) {
        failed.push_back("video send: " + vtx_->error());
        vtx_.reset();
    }

    active_ = atx_ || vtx_;
    title_ = title;
    group_ = group;
    started_us_ = proto::now_us();
    return failed;
}

void BroadcastSession::stop() {
    if (!active_) return;
    summary_.clear();
    if (vtx_) {
        vtx_->stop();
        summary_.push_back(vtx_->stats());
    }
    if (atx_) {
        atx_->stop();
        summary_.push_back(atx_->stats());
    }
    vtx_.reset();
    atx_.reset();
    active_ = false;
    bridges_.clear();
}

void BroadcastSession::send_to_all() {
    const std::string g = group_ip();
    std::vector<proto::Endpoint> audio = {{g, proto::kAudioPort}};
    std::vector<proto::Endpoint> video = {{g, proto::kVideoPort}};
    for (const std::string& b : bridges_) {  // a bridge takes both on its one port
        audio.push_back({b, net::kBridgePort});
        video.push_back({b, net::kBridgePort});
    }
    if (atx_) atx_->set_destinations(audio);
    if (vtx_) vtx_->set_destinations(video);
}

bool BroadcastSession::add_bridge(const std::string& ip) {
    in_addr a{};
    if (!active_ || inet_pton(AF_INET, ip.c_str(), &a) != 1 || proto::is_multicast_ipv4(ip))
        return false;
    if (std::find(bridges_.begin(), bridges_.end(), ip) != bridges_.end()) return false;
    bridges_.push_back(ip);
    send_to_all();
    return true;
}

bool BroadcastSession::remove_bridge(const std::string& ip) {
    const auto it = std::find(bridges_.begin(), bridges_.end(), ip);
    if (it == bridges_.end()) return false;
    bridges_.erase(it);
    send_to_all();
    return true;
}

std::string BroadcastSession::group_ip() const {
    return proto::lancall_group(group_);
}

std::string BroadcastSession::parts() const {
    return parts_of(atx_ != nullptr, vtx_ != nullptr);
}

bool BroadcastSession::self_view(cv::Mat& out) const {
    return vtx_ && vtx_->self_view(out);
}

std::vector<std::string> BroadcastSession::stats() const {
    std::vector<std::string> lines;
    if (atx_) lines.push_back(atx_->stats());
    if (vtx_) lines.push_back(vtx_->stats());
    return lines;
}

// ---- viewer --------------------------------------------------------------------------------

WatchSession::~WatchSession() {
    stop();
}

std::vector<std::string> WatchSession::start(const std::string& name, const std::string& title,
                                             const std::string& presenter_ip, uint8_t group,
                                             const WatchOptions& opt) {
    stop();
    std::vector<std::string> failed;
    const std::string g = proto::lancall_group(group);
    if (g.empty()) return {"the broadcast has no multicast group"};

    // only_from = the presenter: anything else sent to the group (or to us) is ignored
    arx_ = std::make_unique<audio::AudioReceiver>();
    audio::ReceiverOptions ao;
    ao.out_device = opt.out_device;
    ao.buffer_ms = opt.buffer_ms;
    if (!arx_->start(proto::kAudioPort, presenter_ip, ao)) {
        failed.push_back("audio receive: " + arx_->error());
        arx_.reset();
    } else if (!arx_->join_group(g, opt.iface_ip)) {
        failed.push_back(arx_->error());
        arx_->stop();
        arx_.reset();
    }

    vrx_ = std::make_unique<video::VideoReceiver>();
    if (!vrx_->start(proto::kVideoPort, presenter_ip)) {
        failed.push_back("video receive: " + vrx_->error());
        vrx_.reset();
    } else if (!vrx_->join_group(g, opt.iface_ip)) {
        failed.push_back("video receive: " + vrx_->error());
        vrx_->stop();
        vrx_.reset();
    } else {
        vrx_->set_nack(false);  // the presenter doesn't resend; FEC repairs what it can
    }

    active_ = arx_ || vrx_;
    name_ = name;
    title_ = title;
    presenter_ip_ = presenter_ip;
    iface_ip_ = opt.iface_ip;
    group_ = group;
    return failed;
}

void WatchSession::stop() {
    if (!active_) return;
    summary_.clear();
    if (vrx_) {
        vrx_->stop();  // leaves the group too
        summary_.push_back(vrx_->stats());
    }
    if (arx_) {
        arx_->stop();
        summary_.push_back(arx_->stats());
    }
    vrx_.reset();
    arx_.reset();
    active_ = false;
}

bool WatchSession::move_to(uint8_t group) {
    const std::string g = proto::lancall_group(group);
    if (!active_ || g.empty()) return false;
    bool ok = true;
    if (arx_) ok = arx_->join_group(g, iface_ip_) && ok;  // joining one leaves the other
    if (vrx_) ok = vrx_->join_group(g, iface_ip_) && ok;
    group_ = group;
    return ok;
}

std::string WatchSession::parts() const {
    return parts_of(arx_ != nullptr, vrx_ != nullptr);
}

bool WatchSession::audio_clock(uint32_t& out) const {
    return arx_ && arx_->playing_capture_us(out);
}

bool WatchSession::audio_clock(uint8_t stream_id, uint32_t& out) const {
    return arx_ && arx_->playing_capture_us(stream_id, out);
}

std::vector<uint8_t> WatchSession::audio_streams() const {
    return arx_ ? arx_->active_streams() : std::vector<uint8_t>{};
}

bool WatchSession::pop_frame(video::VideoFrame& out) {
    return vrx_ && vrx_->pop(out);
}

std::vector<std::string> WatchSession::stats() const {
    std::vector<std::string> lines;
    if (arx_) lines.push_back(arx_->stats());
    if (vrx_) lines.push_back(vrx_->stats());
    return lines;
}

}  // namespace call
