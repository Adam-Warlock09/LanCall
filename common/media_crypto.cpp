// Media encryption (phase 6) with libsodium's XChaCha20-Poly1305. The format is described in
// media_crypto.hpp. XChaCha's 24-byte nonce is big enough to pick at random for every
// datagram, so there's no counter to keep in step between senders, resends and the relay.
#include "media_crypto.hpp"

#include <sodium.h>

#include <cstring>

#include "protocol.hpp"

namespace proto {

namespace {

constexpr size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr size_t kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;
static_assert(kNonce + kTag == MediaCrypto::kOverhead, "format in media_crypto.hpp");
static_assert(MediaCrypto::kKeyBytes == crypto_aead_xchacha20poly1305_ietf_KEYBYTES, "key size");

// sodium_init() may be called any number of times from any thread
bool ready() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

// media bodies are secret; reports, NACKs and keepalives are only signed (the relay reads them)
bool body_is_secret(uint8_t type) {
    return type == static_cast<uint8_t>(Type::Video) || type == static_cast<uint8_t>(Type::Audio) ||
           type == static_cast<uint8_t>(Type::VideoFec);
}

}  // namespace

MediaCrypto::MediaCrypto(const Key& key) : key_(key) {
    ready();
}

MediaCrypto::~MediaCrypto() {
    sodium_memzero(key_.data(), key_.size());
}

bool MediaCrypto::seal(const uint8_t* in, size_t len, std::vector<uint8_t>& out) const {
    if (len < kHeaderSize || !ready()) return false;
    const size_t body = len - kHeaderSize;
    out.resize(len + kOverhead);
    std::memcpy(out.data(), in, kHeaderSize);
    uint8_t* nonce = out.data() + len;
    uint8_t* tag = nonce + kNonce;
    randombytes_buf(nonce, kNonce);

    unsigned long long tag_len = 0;
    int r;
    if (body_is_secret(in[0])) {
        // the header is authenticated but stays readable
        r = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
            out.data() + kHeaderSize, tag, &tag_len, in + kHeaderSize, body, in, kHeaderSize,
            nullptr, nonce, key_.data());
    } else {
        // nothing to hide: header and body are both just authenticated
        std::memcpy(out.data() + kHeaderSize, in + kHeaderSize, body);
        uint8_t none = 0;
        r = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(&none, tag, &tag_len, &none, 0, in,
                                                                len, nullptr, nonce, key_.data());
    }
    return r == 0 && tag_len == kTag;
}

bool MediaCrypto::open(const uint8_t* in, size_t len, std::vector<uint8_t>& out) const {
    if (len < kHeaderSize + kOverhead || !ready()) return false;
    const size_t body = len - kHeaderSize - kOverhead;
    const uint8_t* nonce = in + kHeaderSize + body;
    const uint8_t* tag = nonce + kNonce;
    out.resize(kHeaderSize + body);
    std::memcpy(out.data(), in, kHeaderSize);

    int r;
    if (body_is_secret(in[0])) {
        r = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(out.data() + kHeaderSize, nullptr,
                                                                in + kHeaderSize, body, tag, in,
                                                                kHeaderSize, nonce, key_.data());
    } else {
        uint8_t none = 0;
        r = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
            &none, nullptr, &none, 0, tag, in, kHeaderSize + body, nonce, key_.data());
        if (r == 0) std::memcpy(out.data() + kHeaderSize, in + kHeaderSize, body);
    }
    if (r != 0) out.clear();
    return r == 0;
}

bool MediaCrypto::available() {
    return ready();
}

}  // namespace proto
