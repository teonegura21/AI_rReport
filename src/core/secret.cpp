#include <rr/core/secret.hpp>

#include <openssl/crypto.h>

namespace rr {

Secret::Secret(std::string value) noexcept : value_(std::move(value)) {}

Secret::Secret(const Secret& other) = default;

Secret& Secret::operator=(const Secret& other) {
    if (this != &other) {
        wipe();
        value_ = other.value_;
    }
    return *this;
}

Secret::Secret(Secret&& other) noexcept : value_(std::move(other.value_)) {
    // A moved-from std::string may keep its bytes (small-string optimization
    // copies instead of stealing), so the source is wiped explicitly.
    other.wipe();
}

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) {
        wipe();
        value_ = std::move(other.value_);
        other.wipe();
    }
    return *this;
}

Secret::~Secret() {
    wipe();
}

bool Secret::matches(std::string_view candidate) const noexcept {
    // The length itself is not secret (tokens have a fixed format), but the
    // bytes are: CRYPTO_memcmp takes the same time regardless of where they differ.
    if (candidate.size() != value_.size()) {
        return false;
    }
    return CRYPTO_memcmp(candidate.data(), value_.data(), value_.size()) == 0;
}

void Secret::wipe() noexcept {
    if (!value_.empty()) {
        // OPENSSL_cleanse cannot be optimized away, unlike a plain memset.
        OPENSSL_cleanse(value_.data(), value_.size());
    }
    value_.clear();
}

}  // namespace rr
