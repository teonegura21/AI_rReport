#include <rr/core/random.hpp>

#include <climits>
#include <vector>

#include <openssl/rand.h>

#include <rr/core/hash.hpp>

namespace rr {

Result<void> random_bytes(std::span<std::byte> out) {
    // RAND_bytes takes an int length; request in chunks to support any size.
    constexpr std::size_t kMaxChunk = INT_MAX;
    for (std::size_t offset = 0; offset < out.size(); offset += kMaxChunk) {
        const std::size_t chunk = std::min(kMaxChunk, out.size() - offset);
        if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data() + offset), static_cast<int>(chunk)) != 1) {
            return fail(Errc::internal, "the system random number generator failed (RAND_bytes)");
        }
    }
    return ok();
}

Result<std::string> random_hex(std::size_t byte_count) {
    std::vector<std::byte> bytes(byte_count);
    RR_TRY(random_bytes(bytes));
    return to_hex(bytes);
}

}  // namespace rr
