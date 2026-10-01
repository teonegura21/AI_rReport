// rr/hub/army_engines.hpp — the army's engines, from what the pod reports and
// how the configuration says each family samples.
//
// Why this exists
//   The army (rr/agents/army.hpp) needs, for each engine:
//     * what it holds: slots, context, context per sequence, shared pool or
//       not. The runtime reports these in its health report (protocol v2), as
//       the engine itself counts them;
//     * its model family, from the catalog id (agents::family_of());
//     * how it samples, directly and thinking: the hub's
//       [sampling.<family>_direct] and [sampling.<family>_thinking] presets.
//   army_engines() puts them together, with three rules:
//     * the army's engines serve the commander and the infantry. The
//       embedder and the reranker are not called with missions;
//     * a pod serving one of the two roles serves both from that engine. The
//       24 GB profile runs the commander alone, the 16 GB profile light
//       infantry alone, and the pipeline still runs, at the quality its one
//       model allows;
//     * an engine that is not ready, a role served twice, or a family without
//       its presets is an error that says so.
//
// Thread-safety: a pure function.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <rr/agents/mission.hpp>
#include <rr/core/result.hpp>
#include <rr/llm/sampling.hpp>
#include <rr/runtime_api/protocol.hpp>

namespace rr::hub {

// The engines missions can be sent to (see the header comment). Errors:
//   Errc::unavailable    an engine of the army is not ready yet;
//   Errc::unsupported    the pod serves neither the commander nor the infantry;
//   Errc::invalid_argument  a role served by two engines, or a family without
//                        its presets (the message names the [sampling.*] table).
[[nodiscard]] Result<std::vector<agents::Engine>> army_engines(
    const runtime_api::HealthReport& report,
    const std::map<std::string, llm::SamplingPreset, std::less<>>& presets);

}  // namespace rr::hub
