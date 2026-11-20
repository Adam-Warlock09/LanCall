#pragma once
// Encrypted text lines for signaling (Person 1, control/, phase 6). Not frozen.
//
// When two lancalls connect, each makes a fresh X25519 key pair and sends its public key in
// the clear (HELLO pk=...). From the two public keys both sides derive the same pair of
// session keys (libsodium crypto_kx: one for each direction), and from then on every line
// goes out as
//
//   S <hex of the encrypted line + 16-byte tag>
//
// XChaCha20-Poly1305 with a counter as the nonce, one counter per direction: TCP keeps lines
// in order, so a line that doesn't open with the next counter was altered, replayed, dropped
// or inserted, and the connection is closed.
//
// No certificates, so a man in the middle could do the handshake with both sides. The
// safety code catches that: it's a hash of both public keys, the same on both screens only
// if nobody sits in between. People read it to each other, like Signal's safety numbers.
#include <array>
#include <cstdint>
#include <string>

namespace control {

class SecureLine {
public:
    static constexpr size_t kKeyBytes = 32;

    SecureLine();   // a fresh key pair every time
    ~SecureLine();  // wipes the keys

    std::string public_hex() const;  // 64 hex characters, for HELLO

    // the other side's public key arrived. we_called = we opened the connection. false if
    // the key isn't 64 hex characters, or is unusable
    bool start(const std::string& their_hex, bool we_called);
    bool ready() const { return ready_; }

    // one line (no newline in it) -> hex for "S <hex>"
    std::string seal(const std::string& line);
    // hex from "S <hex>" -> the line. false if it doesn't open with the next counter
    bool open(const std::string& hex, std::string& line);

    // "1234 5678": both sides show the same code unless someone is in between. "" before start
    std::string safety_code() const;

    static bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t n);
    static std::string bytes_to_hex(const uint8_t* in, size_t n);

private:
    std::array<uint8_t, kKeyBytes> pk_{}, sk_{}, their_pk_{};
    std::array<uint8_t, kKeyBytes> rx_{}, tx_{};
    uint64_t rx_count_ = 0, tx_count_ = 0;
    bool we_called_ = false;
    bool ready_ = false;
};

}  // namespace control
