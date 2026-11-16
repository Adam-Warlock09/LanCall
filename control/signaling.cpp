// PLACEHOLDER from the phase 2 kickoff, so call/ links from day one.
// Person 1 replaces this with the real TCP state machine and clock sync.
#include "control/signaling.hpp"

namespace control {

struct Signaling::Impl {};

Signaling::Signaling() : impl_(std::make_unique<Impl>()) {}
Signaling::~Signaling() = default;
bool Signaling::start(const std::string&, uint16_t) {
    return false;
}
void Signaling::stop() {}
bool Signaling::call(const std::string&, uint16_t) {
    return false;
}
void Signaling::accept() {}
void Signaling::decline() {}
void Signaling::hang_up() {}
CallState Signaling::state() const {
    return CallState::Idle;
}
std::string Signaling::peer_name() const {
    return {};
}
std::string Signaling::peer_ip() const {
    return {};
}
bool Signaling::poll_event(CallEvent&) {
    return false;
}
bool Signaling::clock_offset(uint32_t&) const {
    return false;
}
double Signaling::rtt_ms() const {
    return 0.0;
}
std::string Signaling::error() const {
    return "signaling not implemented yet";
}

}  // namespace control