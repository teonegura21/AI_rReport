// rr/core/random.hpp — cryptographically secure randomness.
//
// Why this exists
//   Bearer tokens for the runtime, pairing codes and nonces must be
//   unpredictable. std::mt19937 is *not* (its state can be reconstructed from
//   outputs). These functions use the operating system's CSPRNG through
//   OpenSSL's RAND_bytes.
//
// Thread-safety: thread-safe.
#pragma once

#include <cstddef>
#include <span>
#include <string>

#include <rr/core/result.hpp>

namespace rr {

// Fills `out` with secure random bytes.
[[nodiscard]] Result<void> random_bytes(std::span<std::byte> out);

// `byte_count` secure random bytes, hex-encoded (so 2 * byte_count characters).
// 32 bytes (256 bits) is the recommended size for tokens.
[[nodiscard]] Result<std::string> random_hex(std::size_t byte_count = 32);

}  // namespace rr
