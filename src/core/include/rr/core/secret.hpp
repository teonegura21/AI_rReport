// rr/core/secret.hpp — credentials that never leak into logs.
//
// Why this exists
//   API keys and bearer tokens travel through configuration, HTTP headers and
//   error messages. A plain std::string ends up in a log line sooner or later.
//   `Secret`:
//     * has no implicit conversion to a string, and formats as "***";
//     * requires an explicit, greppable `.expose()` to read the value;
//     * wipes its memory on destruction and after a move;
//     * compares in constant time (`matches`), so checking a bearer token does
//       not leak its prefix through timing.
//
// Thread-safety: a value type; distinct objects are independent.
#pragma once

#include <format>
#include <ostream>
#include <string>
#include <string_view>

namespace rr {

class Secret {
public:
    Secret() = default;
    explicit Secret(std::string value) noexcept;
    Secret(const Secret& other);
    Secret& operator=(const Secret& other);
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    ~Secret();

    // The raw value. Every call site is a place where the secret leaves this
    // type, which is why the name is deliberately explicit.
    [[nodiscard]] std::string_view expose() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

    // Constant-time comparison with a candidate value (e.g. a request's bearer token).
    [[nodiscard]] bool matches(std::string_view candidate) const noexcept;

private:
    void wipe() noexcept;

    std::string value_;
};

inline std::ostream& operator<<(std::ostream& out, const Secret& /*secret*/) {
    return out << "***";
}

}  // namespace rr

template <class CharT>
struct std::formatter<rr::Secret, CharT> : std::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(const rr::Secret& /*secret*/, FormatContext& context) const {
        return std::formatter<std::string_view, CharT>::format("***", context);
    }
};
