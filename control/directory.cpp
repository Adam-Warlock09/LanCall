// PLACEHOLDER from the phase 3 kickoff, so everything builds from day one.
// Person 1 replaces this with the real directory.
#include "control/directory.hpp"

namespace control {

struct DirectoryServer::Impl {};

DirectoryServer::DirectoryServer() : impl_(std::make_unique<Impl>()) {}
DirectoryServer::~DirectoryServer() = default;
bool DirectoryServer::start(uint16_t) {
    return false;
}
void DirectoryServer::stop() {}
bool DirectoryServer::running() const {
    return false;
}
size_t DirectoryServer::peer_count() const {
    return 0;
}
std::string DirectoryServer::table() const {
    return {};
}
std::string DirectoryServer::error() const {
    return "directory not implemented yet";
}

}  // namespace control