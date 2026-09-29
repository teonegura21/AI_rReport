#include <rr/core/assert.hpp>

#include <cstdio>
#include <cstdlib>

namespace rr::detail {

void assertion_failed(const char* expression, std::string_view message, std::source_location where) noexcept {
    // fprintf rather than iostreams or a logger: this must work even when the
    // failing invariant lives inside the logging machinery itself.
    (void)std::fprintf(stderr, "\n[rReport] assertion failed: %s\n  message:  %.*s\n  location: %s:%u (%s)\n",
                       expression, static_cast<int>(message.size()), message.data(), where.file_name(),
                       static_cast<unsigned>(where.line()), where.function_name());
    (void)std::fflush(stderr);
    std::abort();
}

}  // namespace rr::detail
