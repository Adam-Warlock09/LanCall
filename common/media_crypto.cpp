// Stand-in from the phase 6/7 kickoff: copies datagrams through unchanged. Aric's phase 6
// branch replaces this file with the real thing (libsodium).
#include "media_crypto.hpp"

#include "protocol.hpp"

namespace proto {

MediaCrypto::MediaCrypto(const Key& key) : key_(key) {}

MediaCrypto::~MediaCrypto() {
    key_.fill(0);
}

bool MediaCrypto::seal(const uint8_t* in, size_t len, std::vector<uint8_t>& out) const {
    if (len < kHeaderSize) return false;
    out.assign(in, in + len);
    return true;
}

bool MediaCrypto::open(const uint8_t* in, size_t len, std::vector<uint8_t>& out) const {
    if (len < kHeaderSize) return false;
    out.assign(in, in + len);
    return true;
}

bool MediaCrypto::available() {
    return false;
}

}  // namespace proto
