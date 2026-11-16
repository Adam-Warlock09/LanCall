// Unit tests for metrics/latency.hpp: E1 delay and lip-sync offset arithmetic mod 2^32.

#include "metrics/latency.hpp"

#include <cstdint>
#include <initializer_list>

#include "check.hpp"

using metrics::av_offset_us;
using metrics::e2e_us;
using metrics::time_after;

namespace {

constexpr uint64_t kHourUs = 3600ull * 1000 * 1000;

// Offset for a peer whose clock reads `peer_minus_local_us` more than ours (may be negative),
// reduced mod 2^32 the way Signaling::clock_offset_us() reports it.
uint32_t offset_for(int64_t peer_minus_local_us) {
    return static_cast<uint32_t>(static_cast<uint64_t>(peer_minus_local_us));
}

// Local "now" values spread over the 32-bit range, including both sides of the wrap.
const uint32_t kLocalTimes[] = {0u,          1000u,       123456789u, 0x7FFFFFFFu,
                                0x80000000u, 0xFFFF0000u, 0xFFFFFFFFu};

void test_same_laptop() {
    CHECK(e2e_us(1000000, 0, 900000) == 100000);
    CHECK(e2e_us(5, 0, 5) == 0);
}

void test_peer_hours_ahead_and_behind() {
    // Clock offsets of +-10 h (3.6e10 us) are far outside int32 range; results must still be exact.
    for (int64_t diff : {static_cast<int64_t>(10 * kHourUs), -static_cast<int64_t>(10 * kHourUs),
                         static_cast<int64_t>(37 * kHourUs) + 12345, int64_t{-1}}) {
        const uint32_t offset = offset_for(diff);
        for (uint32_t local : kLocalTimes) {
            // Captured 120 ms before `local`, stamped on the peer's clock.
            const uint32_t capture = static_cast<uint32_t>(static_cast<uint64_t>(local) +
                                                           static_cast<uint64_t>(diff) - 120000);
            CHECK(e2e_us(local, offset, capture) == 120000);
        }
    }
}

void test_wrap_between_capture_and_display() {
    // Sender clock wraps between capture and display: 0xFFFFFF00 -> 0x00000100 (+512 us).
    CHECK(e2e_us(0x00000100u, 0, 0xFFFFFF00u) == 512);
    // Our clock wraps, the peer's does not.
    const uint32_t offset = offset_for(-static_cast<int64_t>(2 * kHourUs));
    const uint32_t local = 50;  // just after our wrap
    const uint32_t capture = static_cast<uint32_t>(local + offset - 75000u);
    CHECK(e2e_us(local, offset, capture) == 75000);
}

void test_small_negative_is_kept() {
    // An offset estimate slightly off can make a delay look negative; it must stay negative,
    // not turn into ~4295 seconds.
    CHECK(e2e_us(1000000, 0, 1002000) == -2000);
    CHECK(e2e_us(10, 0, 0xFFFFFFF0u + 30) == -4);
}

void test_av_offset() {
    CHECK(av_offset_us(2000000, 1900000) == 100000);      // video 100 ms ahead of audio
    CHECK(av_offset_us(1900000, 2000000) == -100000);     // video behind
    CHECK(av_offset_us(0x00000010u, 0xFFFFFFF0u) == 32);  // across the wrap
}

void test_time_after() {
    CHECK(time_after(10, 5));
    CHECK(!time_after(5, 10));
    CHECK(!time_after(7, 7));
    CHECK(time_after(0x00000005u, 0xFFFFFFF0u));  // just after the wrap is later
    CHECK(!time_after(0xFFFFFFF0u, 0x00000005u));
}

void test_us_to_ms() {
    CHECK_NEAR(metrics::us_to_ms(120500), 120.5, 1e-12);
    CHECK_NEAR(metrics::us_to_ms(-2000), -2.0, 1e-12);
}

}  // namespace

int main() {
    RUN(test_same_laptop);
    RUN(test_peer_hours_ahead_and_behind);
    RUN(test_wrap_between_capture_and_display);
    RUN(test_small_negative_is_kept);
    RUN(test_av_offset);
    RUN(test_time_after);
    RUN(test_us_to_ms);
    return check::result();
}
