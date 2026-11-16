// PLACEHOLDER from the phase 2 kickoff, so call/ links from day one.
// Person 1 replaces this with the real beacon sender and listener.
#include "control/discovery.hpp"

namespace control {

struct Discovery::Impl {};

Discovery::Discovery() : impl_(std::make_unique<Impl>()) {}
Discovery::~Discovery() = default;
bool Discovery::start(const std::string&, uint16_t) {
    return false;
}
void Discovery::stop() {}
std::vector<Peer> Discovery::peers() const {
    return {};
}
std::string Discovery::error() const {
    return "discovery not implemented yet";
}

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

}  // namespace control