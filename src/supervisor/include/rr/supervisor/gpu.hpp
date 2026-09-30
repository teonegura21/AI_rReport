// rr/supervisor/gpu.hpp — which GPUs this pod has, according to nvidia-smi.
//
// Why this exists
//   The runtime picks its GPU profile from the available VRAM, and /v1/health
//   reports memory and utilization so the hub (and you) can see what the pod
//   is doing. nvidia-smi is the one tool present wherever the NVIDIA driver
//   is; its CSV query output is stable and documented, and it keeps working
//   inside containers, where NVML access can be restricted.
//
// Thread-safety: pure parsing; query_gpus() runs a subprocess.
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/runtime_api/protocol.hpp>

namespace rr::supervisor {

// The query query_gpus() runs; its output is what parse_nvidia_smi() reads.
inline constexpr std::string_view kNvidiaSmiQuery =
    "--query-gpu=index,name,memory.total,memory.used,utilization.gpu,driver_version";
inline constexpr std::string_view kNvidiaSmiFormat = "--format=csv,noheader,nounits";

// Parses nvidia-smi's CSV (no header, no units): one GPU per line. Fields
// the driver cannot report ("[N/A]", "[Not Supported]": MIG, some virtual
// GPUs) read as 0.
[[nodiscard]] Result<std::vector<runtime_api::GpuInfo>> parse_nvidia_smi(std::string_view csv);

// Runs nvidia-smi and parses its answer. Errc::unavailable when there is no
// usable GPU (no nvidia-smi, no driver, or it fails).
[[nodiscard]] Result<std::vector<runtime_api::GpuInfo>> query_gpus(
    const std::string& executable = "nvidia-smi",
    std::chrono::milliseconds timeout = std::chrono::seconds{15});

}  // namespace rr::supervisor
