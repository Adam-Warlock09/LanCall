// Placeholder from the phase 4 kickoff so everything builds; Aric's phase 4 branch replaces
// this file with the real relay.
#include "net/relay.hpp"

#include <mutex>

namespace net {

struct Relay::Impl {
    mutable std::mutex mu;
    std::string error;
};

Relay::Relay() : impl_(std::make_unique<Impl>()) {}
Relay::~Relay() {
    stop();
}

bool Relay::start(const RelayOptions& opt) {
    (void)opt;
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->error = "the relay isn't written yet (phase 4)";
    return false;
}

void Relay::stop() {}
bool Relay::running() const {
    return false;
}
std::vector<RelayMember> Relay::members() const {
    return {};
}
std::string Relay::stats() const {
    return "relay off";
}

std::string Relay::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

}  // namespace net
