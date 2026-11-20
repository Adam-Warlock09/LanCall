#include "control/secure_line.hpp"

#include <sodium.h>

#include <cstdio>
#include <vector>

namespace control {

namespace {

constexpr size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr size_t kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;
static_assert(SecureLine::kKeyBytes == crypto_kx_PUBLICKEYBYTES, "kx key size");
static_assert(SecureLine::kKeyBytes == crypto_kx_SESSIONKEYBYTES, "session key size");

bool sodium_ready() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

// the line counter, little-endian, padded with zeros to a nonce
void counter_nonce(uint64_t n, uint8_t* nonce) {
    for (size_t i = 0; i < kNonce; ++i) nonce[i] = 0;
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<uint8_t>(n >> (8 * i));
}

}  // namespace

SecureLine::SecureLine() {
    if (sodium_ready()) crypto_kx_keypair(pk_.data(), sk_.data());
}

SecureLine::~SecureLine() {
    sodium_memzero(sk_.data(), sk_.size());
    sodium_memzero(rx_.data(), rx_.size());
    sodium_memzero(tx_.data(), tx_.size());
}

std::string SecureLine::public_hex() const {
    return bytes_to_hex(pk_.data(), pk_.size());
}

bool SecureLine::start(const std::string& their_hex, bool we_called) {
    if (!sodium_ready() || ready_ || !hex_to_bytes(their_hex, their_pk_.data(), kKeyBytes))
        return false;
    int r = we_called ? crypto_kx_client_session_keys(rx_.data(), tx_.data(), pk_.data(),
                                                      sk_.data(), their_pk_.data())
                      : crypto_kx_server_session_keys(rx_.data(), tx_.data(), pk_.data(),
                                                      sk_.data(), their_pk_.data());
    if (r != 0) return false;  // a key that would give a weak shared secret
    we_called_ = we_called;
    ready_ = true;
    return true;
}

std::string SecureLine::seal(const std::string& line) {
    if (!ready_) return "";
    std::vector<uint8_t> out(line.size() + kTag);
    uint8_t nonce[kNonce];
    counter_nonce(tx_count_++, nonce);
    unsigned long long tag_len = 0;
    uint8_t none = 0;
    const uint8_t* m = line.empty() ? &none : reinterpret_cast<const uint8_t*>(line.data());
    crypto_aead_xchacha20poly1305_ietf_encrypt_detached(out.data(), out.data() + line.size(),
                                                        &tag_len, m, line.size(), nullptr, 0,
                                                        nullptr, nonce, tx_.data());
    return bytes_to_hex(out.data(), out.size());
}

bool SecureLine::open(const std::string& hex, std::string& line) {
    if (!ready_ || hex.size() < 2 * kTag || hex.size() % 2 != 0) return false;
    std::vector<uint8_t> in(hex.size() / 2);
    if (!hex_to_bytes(hex, in.data(), in.size())) return false;
    const size_t len = in.size() - kTag;
    std::vector<uint8_t> plain(len + 1);
    uint8_t nonce[kNonce];
    counter_nonce(rx_count_, nonce);
    if (crypto_aead_xchacha20poly1305_ietf_decrypt_detached(plain.data(), nullptr, in.data(), len,
                                                            in.data() + len, nullptr, 0, nonce,
                                                            rx_.data()) != 0)
        return false;
    rx_count_++;
    line.assign(reinterpret_cast<const char*>(plain.data()), len);
    return true;
}

std::string SecureLine::safety_code() const {
    if (!ready_) return "";
    // caller's key first, so both ends hash the same 64 bytes
    uint8_t both[2 * kKeyBytes];
    const auto& first = we_called_ ? pk_ : their_pk_;
    const auto& second = we_called_ ? their_pk_ : pk_;
    for (size_t i = 0; i < kKeyBytes; ++i) {
        both[i] = first[i];
        both[kKeyBytes + i] = second[i];
    }
    uint8_t h[crypto_generichash_BYTES];
    crypto_generichash(h, sizeof(h), both, sizeof(both), nullptr, 0);
    uint32_t v = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u %04u", (v / 10000) % 10000, v % 10000);
    return buf;
}

bool SecureLine::hex_to_bytes(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() != 2 * n) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        int hi = val(hex[2 * i]), lo = val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

std::string SecureLine::bytes_to_hex(const uint8_t* in, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = digits[in[i] >> 4];
        s[2 * i + 1] = digits[in[i] & 15];
    }
    return s;
}

}  // namespace control
