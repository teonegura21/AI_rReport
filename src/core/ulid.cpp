#include <rr/core/ulid.hpp>

#include <algorithm>
#include <chrono>
#include <ranges>

namespace rr {
namespace {

constexpr std::string_view kCrockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Encodes 128 bits (6 bytes of time + 10 bytes of randomness) as 26 base32
// characters. 26 * 5 = 130 bits, so the first character only carries 3 bits.
std::string encode(std::uint64_t time_ms, const std::array<std::uint8_t, 10>& random) {
    std::array<std::uint8_t, 16> bytes{};
    for (int index = 5; index >= 0; --index) {
        bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(time_ms & 0xFFU);
        time_ms >>= 8U;
    }
    std::ranges::copy(random, bytes.begin() + 6);

    std::string text(26, '0');
    // Walk the 130-bit big-endian number 5 bits at a time from the least
    // significant end, filling characters right to left.
    unsigned int buffer = 0;
    int buffered_bits = 0;
    int position = 25;
    for (int index = 15; index >= 0 && position >= 0; --index) {
        buffer |= static_cast<unsigned int>(bytes[static_cast<std::size_t>(index)])
                  << static_cast<unsigned>(buffered_bits);
        buffered_bits += 8;
        while (buffered_bits >= 5 && position >= 0) {
            text[static_cast<std::size_t>(position--)] = kCrockford[buffer & 0x1FU];
            buffer >>= 5U;
            buffered_bits -= 5;
        }
    }
    if (position >= 0) {
        text[static_cast<std::size_t>(position)] = kCrockford[buffer & 0x1FU];
    }
    return text;
}

// Adds one to an 80-bit big-endian counter. Returns false on overflow.
bool increment(std::array<std::uint8_t, 10>& value) {
    for (std::uint8_t& byte : std::views::reverse(value)) {
        if (++byte != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

UlidGenerator::UlidGenerator(const Clock& clock) : clock_(clock), random_(std::random_device{}()) {}

std::string UlidGenerator::next() {
    const auto since_epoch = clock_.now().time_since_epoch();
    auto now_ms = static_cast<std::uint64_t>(
        std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count()));

    const std::lock_guard lock(mutex_);
    if (now_ms <= last_ms_) {
        // Same millisecond (or the clock moved backwards): keep the last
        // timestamp and increment the random part so ordering is preserved.
        now_ms = last_ms_;
        if (!increment(last_random_)) {
            // 2^80 IDs in one millisecond cannot happen in practice; if it did,
            // borrow the next millisecond rather than repeat an ID.
            ++now_ms;
        }
    } else {
        for (std::size_t index = 0; index < last_random_.size(); index += 8) {
            const std::uint64_t word = random_();
            for (std::size_t byte = 0; byte < 8 && index + byte < last_random_.size(); ++byte) {
                last_random_[index + byte] = static_cast<std::uint8_t>(word >> (byte * 8U));
            }
        }
    }
    last_ms_ = now_ms;
    return encode(now_ms, last_random_);
}

std::string new_ulid() {
    static UlidGenerator generator{SystemClock::instance()};
    return generator.next();
}

}  // namespace rr
