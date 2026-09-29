// rr/core/scope_exit.hpp — run a cleanup action when leaving a scope.
//
// Why this exists
//   C APIs (SQLite, libcurl, OpenSSL) hand out resources that need a matching
//   release call. Where a dedicated RAII wrapper would be overkill (a single
//   temporary handle), ScopeExit guarantees the cleanup runs on every path out
//   of the scope, including early `return fail(...)`:
//
//     curl_slist* headers = build_headers();
//     ScopeExit free_headers{[&] { curl_slist_free_all(headers); }};
//
//   (std::scope_exit is still only in a Technical Specification.)
//
// Thread-safety: confined to the owning scope.
#pragma once

#include <type_traits>
#include <utility>

namespace rr {

template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F action) noexcept(std::is_nothrow_move_constructible_v<F>)
        : action_(std::move(action)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit() {
        if (active_) {
            action_();
        }
    }

    // Cancels the action (e.g. ownership was successfully handed over).
    void release() noexcept { active_ = false; }

private:
    F action_;
    bool active_ = true;
};

template <class F>
ScopeExit(F) -> ScopeExit<F>;

}  // namespace rr
