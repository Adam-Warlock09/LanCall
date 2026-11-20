#pragma once
// Media encryption (phase 6). Seals and opens whole media datagrams with the call's key.
//
// FROZEN after the phase 6/7 kickoff: change only by agreement.
//
// A sealed datagram keeps its 12-byte header readable (the relay forwards by stream_id and
// answers NACKs by seq) and gets 40 bytes longer:
//
//   Video, Audio, VideoFec:     [header 12][body, encrypted ][nonce 24][tag 16]
//   Report, Nack, Keepalive:    [header 12][body, readable  ][nonce 24][tag 16]
//
// XChaCha20-Poly1305 with a random nonce per datagram. The tag covers the header in both
// cases and the body too, so nothing can be altered or forged without the key; reports
// and NACKs stay readable because the relay needs their contents and they say nothing
// private. 12 + 1400 + 40 bytes plus UDP/IP headers is 1480, still under the 1500 MTU.
//
// Nobody calls seal/open by hand: UdpSocket::set_crypto() makes a socket seal everything
// it sends and open everything it receives (dropping what doesn't open), and the media
// pipelines pass their set_crypto() on to their sockets. The relay's and discovery's
// sockets never get a key: the relay forwards sealed datagrams without reading them.
//
// Needs libsodium (sudo apt install libsodium-dev).
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace proto {

class MediaCrypto {
public:
    static constexpr size_t kKeyBytes = 32;
    static constexpr size_t kOverhead = 40;  // nonce + tag, added by seal()
    using Key = std::array<uint8_t, kKeyBytes>;

    explicit MediaCrypto(const Key& key);
    ~MediaCrypto();  // wipes the key
    MediaCrypto(const MediaCrypto&) = delete;
    MediaCrypto& operator=(const MediaCrypto&) = delete;

    // a whole datagram (header + body) in, the sealed datagram out. false if `in` isn't a
    // datagram (shorter than a header)
    bool seal(const uint8_t* in, size_t len, std::vector<uint8_t>& out) const;
    // a sealed datagram in, the original out. false if it was altered, cut short, or
    // sealed with another key
    bool open(const uint8_t* in, size_t len, std::vector<uint8_t>& out) const;

    // false only if libsodium can't start (it couldn't find a source of randomness)
    static bool available();

private:
    Key key_;
};

}  // namespace proto
