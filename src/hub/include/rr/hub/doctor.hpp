// rr/hub/doctor.hpp — what `rreport doctor` checks, as data.
//
// Why this exists
//   A hub needs a data directory it can write, a library database it can
//   open, the tools capture runs (yt-dlp with a JavaScript runtime, ffmpeg),
//   and, for any model work, a pod RunPod will start and a runtime there
//   that knows the hub's token and has its engines ready. doctor checks each
//   in that order, says what it found, and for each problem what to do.
//   Checks produce findings (data), so the rules are tested without a
//   terminal; the command only prints them.
//
//   A failure means the hub cannot do its work until it is fixed; a warning
//   means something will not work yet (no pod configured, a tool missing
//   that capture needs later) or is worth a look (little free space, the
//   runtime is another version).
//
// Thread-safety: run_doctor() is reentrant.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/hub/hub_config.hpp>
#include <rr/net/http_client.hpp>
#include <rr/runtime_api/protocol.hpp>

namespace rr::hub {

enum class Severity : std::uint8_t {
    ok,
    warning,
    failure
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

struct Finding {
    std::string check{};  // "data directory", "runtime", …
    Severity severity = Severity::ok;
    std::string detail{};  // what was found
    std::string advice{};  // what to do; empty when there is nothing to do

    friend bool operator==(const Finding&, const Finding&) = default;
};

struct DoctorInputs {
    const HubSettings* settings = nullptr;
    const net::HttpClient* http = nullptr;
    std::string hub_version{};
    // Where a program is on the PATH (a seam for tests).
    std::function<std::optional<std::filesystem::path>(std::string_view program)> find_program{};
    // Free bytes where the data directory is (a seam for tests; empty: the filesystem's).
    std::function<std::optional<std::uintmax_t>(const std::filesystem::path&)> free_space{};
    std::stop_token stop{};
};

[[nodiscard]] std::vector<Finding> run_doctor(const DoctorInputs& inputs);

// Whether the findings let the hub work: no failures.
[[nodiscard]] bool healthy(const std::vector<Finding>& findings) noexcept;

// "NVIDIA GeForce RTX 4090 (24564 MiB, 20512 used)", one per GPU; "no GPU".
[[nodiscard]] std::string describe_gpus(const std::vector<runtime_api::GpuInfo>& gpus);

// A program on the PATH: the first executable file named `program` in one of
// the directories of `path_variable` (colon-separated, like $PATH).
[[nodiscard]] std::optional<std::filesystem::path> find_on_path(std::string_view program,
                                                                std::string_view path_variable);

}  // namespace rr::hub
