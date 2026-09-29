#include <rr/core/environment.hpp>

#include <string_view>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
#include <unistd.h>  // declares `environ` (glibc, with _GNU_SOURCE)
#endif

namespace rr {

namespace {

char** raw_environment() noexcept {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

}  // namespace

std::vector<EnvironmentVariable> environment_snapshot() {
    std::vector<EnvironmentVariable> variables;
    for (char** entry = raw_environment(); entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        const auto equals = text.find('=');
        if (equals != std::string_view::npos) {
            variables.emplace_back(std::string{text.substr(0, equals)}, std::string{text.substr(equals + 1)});
        }
    }
    return variables;
}

std::optional<std::string> environment_variable(std::string_view name) {
    for (char** entry = raw_environment(); entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        if (text.size() > name.size() && text.starts_with(name) && text[name.size()] == '=') {
            return std::string{text.substr(name.size() + 1)};  // the first match, like getenv()
        }
    }
    return std::nullopt;
}

}  // namespace rr
