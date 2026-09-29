#include <rr/core/error.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <utility>

namespace rr {
namespace {

// Keep in the same order as the Errc enumerators; the static_assert below
// guards against the two drifting apart when a class is added.
constexpr std::array<std::string_view, 17> kErrcNames{
    "invalid_argument",  "not_found",   "conflict",
    "permission_denied", "io",          "network",
    "timeout",           "unavailable", "rate_limited",
    "cancelled",         "refused",     "model_output_invalid",
    "budget_exceeded",   "needs_user",  "unsupported",
    "integrity",         "internal",
};
static_assert(static_cast<std::size_t>(Errc::internal) + 1 == kErrcNames.size(),
              "kErrcNames must list every Errc enumerator in order");

}  // namespace

std::string_view to_string(Errc code) noexcept {
    const auto index = static_cast<std::size_t>(code);
    return index < kErrcNames.size() ? kErrcNames[index] : std::string_view{"unknown"};
}

std::optional<Errc> errc_from_string(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kErrcNames.size(); ++index) {
        if (kErrcNames[index] == name) {
            return static_cast<Errc>(index);
        }
    }
    return std::nullopt;
}

Error::Error(Errc code, std::string message, std::source_location where)
    : code_(code)
    , message_(std::move(message))
    , where_(where) {}

Error Error::with_context(std::string context) && {
    context_.push_back(std::move(context));
    return std::move(*this);
}

Error Error::with_retry_after(std::chrono::milliseconds delay) && {
    retry_after_ = delay;
    return std::move(*this);
}

std::string Error::describe() const {
    std::string text = std::format("{}: {}", to_string(code_), message_);
    if (!context_.empty()) {
        text += " (";
        for (std::size_t index = 0; index < context_.size(); ++index) {
            if (index > 0) {
                text += "; ";
            }
            text += context_[index];
        }
        text += ")";
    }
    if (retry_after_) {
        text += std::format(" [retry after {} ms]", retry_after_->count());
    }
    // File name only: full build paths are noise in logs.
    const std::filesystem::path file{where_.file_name()};
    text += std::format(" [{}:{}]", file.filename().string(), where_.line());
    return text;
}

}  // namespace rr
