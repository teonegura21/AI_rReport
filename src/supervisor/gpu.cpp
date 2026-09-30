#include <rr/supervisor/gpu.hpp>

#include <charconv>
#include <format>

#include <rr/core/strings.hpp>
#include <rr/proc/process.hpp>

namespace rr::supervisor {

namespace {

// A number, or 0 for the placeholders nvidia-smi prints when a value is not
// available ("[N/A]", "[Not Supported]", "N/A").
Result<std::int64_t> parse_quantity(std::string_view field, std::string_view name, std::size_t line) {
    if (field.starts_with('[') || field == "N/A") {
        return 0;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (error != std::errc{} || end != field.data() + field.size()) {
        return fail(Errc::invalid_argument,
                    std::format("nvidia-smi line {}: {} is '{}', not a number", line, name, field));
    }
    return value;
}

}  // namespace

Result<std::vector<runtime_api::GpuInfo>> parse_nvidia_smi(std::string_view csv) {
    std::vector<runtime_api::GpuInfo> gpus;
    std::size_t line_number = 0;
    for (const std::string_view raw_line : split(csv, '\n')) {
        ++line_number;
        const std::string_view line = trim(raw_line);
        if (line.empty()) {
            continue;
        }
        std::vector<std::string_view> fields = split(line, ',');
        if (fields.size() != 6) {
            return fail(Errc::invalid_argument,
                        std::format("nvidia-smi line {}: expected 6 fields, got {}: '{}'", line_number,
                                    fields.size(), line));
        }
        for (std::string_view& field : fields) {
            field = trim(field);
        }
        runtime_api::GpuInfo gpu;
        RR_TRY_ASSIGN(const std::int64_t index, parse_quantity(fields[0], "the index", line_number));
        RR_TRY_ASSIGN(gpu.memory_total_mib, parse_quantity(fields[2], "memory.total", line_number));
        RR_TRY_ASSIGN(gpu.memory_used_mib, parse_quantity(fields[3], "memory.used", line_number));
        RR_TRY_ASSIGN(const std::int64_t utilization,
                      parse_quantity(fields[4], "utilization.gpu", line_number));
        gpu.index = static_cast<int>(index);
        gpu.name = std::string{fields[1]};
        gpu.utilization_percent = static_cast<int>(utilization);
        gpu.driver_version = std::string{fields[5]};
        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

Result<std::vector<runtime_api::GpuInfo>> query_gpus(const std::string& executable,
                                                     std::chrono::milliseconds timeout) {
    proc::RunOptions options;
    options.timeout = timeout;
    auto result = proc::run(
        proc::Command{.argv = {executable, std::string{kNvidiaSmiQuery}, std::string{kNvidiaSmiFormat}}},
        options);
    if (!result.has_value()) {
        return fail(Errc::unavailable, std::format("no GPU information: {}", result.error().describe()));
    }
    if (!result->status.success()) {
        return fail(Errc::unavailable, std::format("nvidia-smi failed ({}): {}", result->status.describe(),
                                                   result->stderr_tail(500)));
    }
    return parse_nvidia_smi(result->stdout_text);
}

}  // namespace rr::supervisor
