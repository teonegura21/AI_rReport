// rr/core/hash.hpp — content hashing: BLAKE3 for addresses, SHA-256 for verification.
//
// Why two algorithms
//   * BLAKE3 is our own content-addressing hash: blob addresses, artifact
//     hashes and stage cache keys (ADR-0004). It is cryptographically strong
//     and several times faster than SHA-256, which matters when hashing
//     hour-long audio files on a laptop.
//   * SHA-256 exists because the outside world speaks it: Hugging Face
//     publishes SHA-256 digests for model files, and the runtime must verify
//     multi-gigabyte downloads against them.
//
//   The two digest types are distinct C++ types, so a SHA-256 manifest entry
//   can never be compared with a BLAKE3 blob address by mistake.
//
// Thread-safety: hasher objects are confined to one thread; the one-shot
//   functions are thread-safe.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include <rr/core/result.hpp>

namespace rr {

// A 32-byte digest tagged with the algorithm that produced it.
template <class Algorithm>
class Digest {
public:
    static constexpr std::size_t kSize = 32;

    Digest() = default;

    explicit Digest(const std::array<std::byte, kSize>& bytes) noexcept : bytes_(bytes) {}

    // Parses 64 lowercase or uppercase hex characters.
    [[nodiscard]] static Result<Digest> from_hex(std::string_view hex);

    // 64 lowercase hex characters.
    [[nodiscard]] std::string hex() const;

    [[nodiscard]] std::span<const std::byte, kSize> bytes() const noexcept { return bytes_; }

    friend bool operator==(const Digest&, const Digest&) = default;
    friend auto operator<=>(const Digest&, const Digest&) = default;

private:
    std::array<std::byte, kSize> bytes_{};
};

struct Blake3Algorithm {};

struct Sha256Algorithm {};

using Blake3Digest = Digest<Blake3Algorithm>;
using Sha256Digest = Digest<Sha256Algorithm>;

// Incremental BLAKE3. finalize() does not consume the state: more data may be
// added afterwards and finalize() called again.
class Blake3Hasher {
public:
    Blake3Hasher();
    ~Blake3Hasher();
    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;
    Blake3Hasher(Blake3Hasher&&) noexcept;
    Blake3Hasher& operator=(Blake3Hasher&&) noexcept;

    void update(std::span<const std::byte> data) noexcept;
    void update(std::string_view text) noexcept;
    [[nodiscard]] Blake3Digest finalize() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Incremental SHA-256 (OpenSSL EVP). finalize() does not consume the state.
class Sha256Hasher {
public:
    Sha256Hasher();
    ~Sha256Hasher();
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    Sha256Hasher(Sha256Hasher&&) noexcept;
    Sha256Hasher& operator=(Sha256Hasher&&) noexcept;

    void update(std::span<const std::byte> data);
    void update(std::string_view text);
    [[nodiscard]] Sha256Digest finalize() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] Blake3Digest blake3(std::string_view data) noexcept;
[[nodiscard]] Blake3Digest blake3(std::span<const std::byte> data) noexcept;
[[nodiscard]] Sha256Digest sha256(std::string_view data);

// Streams a file through the hash in 1 MiB chunks; honors cancellation
// between chunks, so hashing a 20 GB model file can be interrupted.
[[nodiscard]] Result<Blake3Digest> blake3_file(const std::filesystem::path& path,
                                               const std::stop_token& stop = {});
[[nodiscard]] Result<Sha256Digest> sha256_file(const std::filesystem::path& path,
                                               const std::stop_token& stop = {});

// Lowercase hex of arbitrary bytes, and its inverse.
[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::string> from_hex(std::string_view hex);

}  // namespace rr

template <class Algorithm, class CharT>
struct std::formatter<rr::Digest<Algorithm>, CharT> : std::formatter<std::string, CharT> {
    template <class FormatContext>
    auto format(const rr::Digest<Algorithm>& digest, FormatContext& context) const {
        return std::formatter<std::string, CharT>::format(digest.hex(), context);
    }
};
