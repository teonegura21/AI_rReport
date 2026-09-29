// rr/core/strong_id.hpp — distinct ID types that cannot be mixed up.
//
// Why this exists
//   Jobs, analyses, videos, missions and blobs are all identified by strings.
//   With plain std::string, `start(job_id, video_id)` and
//   `start(video_id, job_id)` both compile. StrongId gives each kind of ID its
//   own type at zero runtime cost:
//
//     struct JobTag {};
//     using JobId = rr::StrongId<JobTag>;
//
//   Construction is explicit, so an arbitrary string never silently becomes an
//   ID; `.value()` is the explicit way back out.
//
// Thread-safety: a regular value type.
#pragma once

#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace rr {

template <class Tag, class Rep = std::string>
class StrongId {
public:
    StrongId() = default;

    explicit StrongId(Rep value) noexcept(std::is_nothrow_move_constructible_v<Rep>)
        : value_(std::move(value)) {}
    [[nodiscard]] const Rep& value() const noexcept { return value_; }

    friend bool operator==(const StrongId&, const StrongId&) = default;
    friend auto operator<=>(const StrongId&, const StrongId&) = default;

private:
    Rep value_{};
};

}  // namespace rr

template <class Tag, class Rep>
struct std::hash<rr::StrongId<Tag, Rep>> {
    std::size_t operator()(const rr::StrongId<Tag, Rep>& id) const noexcept {
        return std::hash<Rep>{}(id.value());
    }
};

template <class Tag, class Rep, class CharT>
struct std::formatter<rr::StrongId<Tag, Rep>, CharT> : std::formatter<Rep, CharT> {
    template <class FormatContext>
    auto format(const rr::StrongId<Tag, Rep>& id, FormatContext& context) const {
        return std::formatter<Rep, CharT>::format(id.value(), context);
    }
};
