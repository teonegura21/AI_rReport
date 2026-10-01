// rr/eval/snapshot.hpp — a video's captions and description, read once and
// kept with the eval.
//
// Why this exists
//   YouTube's videos change: captions are corrected, regenerated, taken
//   down. An eval that read them anew each time would measure YouTube as
//   much as rReport (ADR-0019). So `rreport eval fetch` reads each video
//   once, through the pipeline's own source stage (rr/pipeline/source.hpp),
//   and keeps what it read with the eval, in eval/data/<id>/:
//     * <id>.source.json, the source artifact itself, written readably;
//     * snapshot.toml, its hash and where and when it was read.
//   From then on the eval reads the snapshot, never YouTube. `rreport
//   ingest` reads it too: a file named *.source.json is a source read
//   earlier, so a snapshot runs through the same stages as the video would.
//
// Pinned by hash
//   A snapshot is the BLAKE3 hash of its artifact's canonical JSON, which is
//   also the artifact's hash in a library. snapshot.toml keeps it, so a
//   snapshot edited by hand, or damaged, is refused. The set may pin it too
//   (EvalVideo::snapshot): a snapshot fetched again that differs from the
//   pin is refused until the set says otherwise.
//
// What a fetch checks before keeping anything
//   * The captions are of the kind the set says, manual or automatic.
//   * A video the set calls CC BY carries YouTube's Creative Commons line:
//     the eval publishes what it makes of its videos, and needs the right.
//
// Thread-safety: functions over files; two fetches of one video at once
//   are not supported.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <rr/core/result.hpp>
#include <rr/eval/set.hpp>
#include <rr/json/json.hpp>
#include <rr/pipeline/source.hpp>

namespace rr::eval {

struct Snapshot {
    pipeline::Source source{};
    std::string hash{};     // of the source artifact's canonical JSON
    std::string fetched{};  // when it was read: "2026-10-01T21:40:00Z"

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

// eval_root/data/<id>/<id>.source.json
[[nodiscard]] std::filesystem::path snapshot_file(const std::filesystem::path& eval_root,
                                                  std::string_view id);

// The snapshot of `video`, or none when it was never fetched. Errors:
//   Errc::integrity  it is not what its snapshot.toml says;
//   Errc::conflict   it is not what the set pins;
//   Errc::invalid_argument  a file that is not a snapshot.
[[nodiscard]] Result<std::optional<Snapshot>> read_snapshot(const std::filesystem::path& eval_root,
                                                            const EvalVideo& video);

// Checks `artifact`, a source artifact of `video` (see the header comment),
// and keeps it as its snapshot, read at `fetched`. Errors:
//   Errc::invalid_argument  captions of the other kind; a CC BY licence
//                           YouTube does not show; not a source artifact;
//   Errc::conflict          the set pins another snapshot;
//   Errc::io                the files could not be written.
[[nodiscard]] Result<Snapshot> keep_snapshot(const std::filesystem::path& eval_root, const EvalVideo& video,
                                             const json::Json& artifact,
                                             std::chrono::system_clock::time_point fetched);

// Reads `video` through `stage`, the pipeline's source stage (yt-dlp), with
// `work` as its scratch directory, and keeps what it read
// (keep_snapshot()). Errors: the stage's, and keep_snapshot()'s.
[[nodiscard]] Result<Snapshot> fetch_snapshot(const std::filesystem::path& eval_root, const EvalVideo& video,
                                              const pipeline::SourceStage& stage,
                                              const std::filesystem::path& work, const std::stop_token& stop,
                                              std::chrono::system_clock::time_point now);

}  // namespace rr::eval
