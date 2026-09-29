#include <rr/core/hash.hpp>

#include <cstring>
#include <fstream>
#include <vector>

#include <blake3.h>

#include <openssl/evp.h>

#include <rr/core/assert.hpp>

namespace rr {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::size_t kFileChunk = std::size_t{1} << 20U;  // 1 MiB

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

template <class Hasher, class DigestT>
Result<DigestT> hash_file(const std::filesystem::path& path, const std::stop_token& stop) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return fail(Errc::io, std::format("cannot open '{}' for hashing", path.string()));
    }
    Hasher hasher;
    std::vector<char> buffer(kFileChunk);
    while (file) {
        if (stop.stop_requested()) {
            return fail(Errc::cancelled, std::format("hashing '{}' was cancelled", path.string()));
        }
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > 0) {
            hasher.update(std::as_bytes(std::span{buffer.data(), count}));
        }
    }
    if (!file.eof()) {
        return fail(Errc::io, std::format("read error while hashing '{}'", path.string()));
    }
    return hasher.finalize();
}

}  // namespace

// ---------------------------------------------------------------------------
// Hex helpers and Digest
// ---------------------------------------------------------------------------
std::string to_hex(std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        text.push_back(kHexDigits[value >> 4U]);
        text.push_back(kHexDigits[value & 0x0FU]);
    }
    return text;
}

Result<std::string> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return fail(Errc::invalid_argument, "hex string has an odd number of characters");
    }
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const int high = hex_value(hex[index]);
        const int low = hex_value(hex[index + 1]);
        if (high < 0 || low < 0) {
            return fail(Errc::invalid_argument, std::format("invalid hex character near offset {}", index));
        }
        bytes.push_back(static_cast<char>((high << 4) | low));
    }
    return bytes;
}

template <class Algorithm>
Result<Digest<Algorithm>> Digest<Algorithm>::from_hex(std::string_view hex) {
    if (hex.size() != kSize * 2) {
        return fail(Errc::invalid_argument,
                    std::format("digest must be {} hex characters, got {}", kSize * 2, hex.size()));
    }
    RR_TRY_ASSIGN(const std::string raw, rr::from_hex(hex));
    std::array<std::byte, kSize> bytes{};
    std::memcpy(bytes.data(), raw.data(), kSize);
    return Digest{bytes};
}

template <class Algorithm>
std::string Digest<Algorithm>::hex() const {
    return to_hex(bytes_);
}

template class Digest<Blake3Algorithm>;
template class Digest<Sha256Algorithm>;

// ---------------------------------------------------------------------------
// BLAKE3
// ---------------------------------------------------------------------------
struct Blake3Hasher::State {
    blake3_hasher hasher{};
};

Blake3Hasher::Blake3Hasher() : state_(std::make_unique<State>()) {
    blake3_hasher_init(&state_->hasher);
}

Blake3Hasher::~Blake3Hasher() = default;
Blake3Hasher::Blake3Hasher(Blake3Hasher&&) noexcept = default;
Blake3Hasher& Blake3Hasher::operator=(Blake3Hasher&&) noexcept = default;

void Blake3Hasher::update(std::span<const std::byte> data) noexcept {
    RR_ASSERT(state_ != nullptr, "Blake3Hasher used after move");
    blake3_hasher_update(&state_->hasher, data.data(), data.size());
}

void Blake3Hasher::update(std::string_view text) noexcept {
    update(std::as_bytes(std::span{text.data(), text.size()}));
}

Blake3Digest Blake3Hasher::finalize() const noexcept {
    RR_ASSERT(state_ != nullptr, "Blake3Hasher used after move");
    std::array<std::byte, Blake3Digest::kSize> out{};
    blake3_hasher_finalize(&state_->hasher, reinterpret_cast<std::uint8_t*>(out.data()), out.size());
    return Blake3Digest{out};
}

Blake3Digest blake3(std::span<const std::byte> data) noexcept {
    Blake3Hasher hasher;
    hasher.update(data);
    return hasher.finalize();
}

Blake3Digest blake3(std::string_view data) noexcept {
    return blake3(std::as_bytes(std::span{data.data(), data.size()}));
}

Result<Blake3Digest> blake3_file(const std::filesystem::path& path, const std::stop_token& stop) {
    return hash_file<Blake3Hasher, Blake3Digest>(path, stop);
}

// ---------------------------------------------------------------------------
// SHA-256 (OpenSSL EVP)
// ---------------------------------------------------------------------------
struct Sha256Hasher::State {
    struct CtxDeleter {
        void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
    };

    std::unique_ptr<EVP_MD_CTX, CtxDeleter> ctx{EVP_MD_CTX_new()};
};

Sha256Hasher::Sha256Hasher() : state_(std::make_unique<State>()) {
    // Failure here means OpenSSL is out of memory or broken: not recoverable.
    RR_ASSERT(state_->ctx != nullptr, "EVP_MD_CTX_new failed");
    RR_ASSERT(EVP_DigestInit_ex(state_->ctx.get(), EVP_sha256(), nullptr) == 1, "EVP_DigestInit_ex failed");
}

Sha256Hasher::~Sha256Hasher() = default;
Sha256Hasher::Sha256Hasher(Sha256Hasher&&) noexcept = default;
Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&&) noexcept = default;

void Sha256Hasher::update(std::span<const std::byte> data) {
    RR_ASSERT(state_ != nullptr, "Sha256Hasher used after move");
    RR_ASSERT(EVP_DigestUpdate(state_->ctx.get(), data.data(), data.size()) == 1, "EVP_DigestUpdate failed");
}

void Sha256Hasher::update(std::string_view text) {
    update(std::as_bytes(std::span{text.data(), text.size()}));
}

Sha256Digest Sha256Hasher::finalize() const {
    RR_ASSERT(state_ != nullptr, "Sha256Hasher used after move");
    // Finalize a copy so this hasher stays usable (matching Blake3Hasher).
    const State copy;
    RR_ASSERT(copy.ctx != nullptr, "EVP_MD_CTX_new failed");
    RR_ASSERT(EVP_MD_CTX_copy_ex(copy.ctx.get(), state_->ctx.get()) == 1, "EVP_MD_CTX_copy_ex failed");
    std::array<std::byte, Sha256Digest::kSize> out{};
    unsigned int length = 0;
    RR_ASSERT(EVP_DigestFinal_ex(copy.ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &length) == 1,
              "EVP_DigestFinal_ex failed");
    RR_ASSERT(length == out.size(), "unexpected SHA-256 digest length");
    return Sha256Digest{out};
}

Sha256Digest sha256(std::string_view data) {
    Sha256Hasher hasher;
    hasher.update(data);
    return hasher.finalize();
}

Result<Sha256Digest> sha256_file(const std::filesystem::path& path, const std::stop_token& stop) {
    return hash_file<Sha256Hasher, Sha256Digest>(path, stop);
}

}  // namespace rr
