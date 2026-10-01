// A video's captions and description, kept with the eval (rr/eval/snapshot.hpp).
#include <rr/eval/snapshot.hpp>

#include <format>
#include <system_error>
#include <utility>

#include <rr/core/file_io.hpp>

#include "toml_io.hpp"

namespace rr::eval {

namespace {

constexpr std::string_view kSnapshotToml = "snapshot.toml";

std::filesystem::path directory_of(const std::filesystem::path& eval_root, std::string_view id) {
    return eval_root / "data" / id;
}

// snapshot.toml: what the snapshot beside it must hash to, and where and
// when it was read.
struct Record {
    std::string hash{};
    std::string fetched{};
    std::string url{};
};

Result<Record> read_record(const std::filesystem::path& file) {
    RR_TRY_ASSIGN(const std::string text, read_file(file));
    RR_TRY_ASSIGN(const toml::table document, detail::parse_toml(text, file.string()));
    detail::Table table{document, file.string()};
    Record record;
    RR_TRY_ASSIGN(record.hash, table.text("hash"));
    RR_TRY_ASSIGN(record.fetched, table.text("fetched"));
    RR_TRY_ASSIGN(record.url, table.text("url"));
    RR_TRY(table.nothing_else());
    return record;
}

// The checks a fetch makes before keeping a source (see the header comment).
Result<void> check_against_set(const EvalVideo& video, const pipeline::Source& source) {
    if (source.captions.kind != video.captions) {
        return fail(
            Errc::invalid_argument,
            std::format("{}: the set says its captions are {}, and YouTube gives {} ones: correct the "
                        "set, or choose another video",
                        video.id, media::to_string(video.captions),
                        source.captions.kind.has_value() ? media::to_string(*source.captions.kind)
                                                         : std::string_view{"unknown"}));
    }
    const bool creative_commons =
        source.video.license.has_value() && source.video.license->contains("Creative Commons");
    if (video.licence == Licence::cc_by && !creative_commons) {
        return fail(
            Errc::invalid_argument,
            std::format("{}: the set says CC BY, and YouTube shows {}: only a licence the page states "
                        "can be relied on",
                        video.id, source.video.license.value_or("no licence")));
    }
    return ok();
}

Result<void> check_pin(const EvalVideo& video, std::string_view hash) {
    if (video.snapshot.has_value() && *video.snapshot != hash) {
        return fail(Errc::conflict,
                    std::format("{}: the set pins the snapshot {}, and this one is {}: YouTube's captions or "
                                "description changed. Keep the old snapshot, or pin the new one in the set",
                                video.id, *video.snapshot, hash));
    }
    return ok();
}

}  // namespace

std::filesystem::path snapshot_file(const std::filesystem::path& eval_root, std::string_view id) {
    return directory_of(eval_root, id) / std::format("{}.source.json", id);
}

Result<std::optional<Snapshot>> read_snapshot(const std::filesystem::path& eval_root,
                                              const EvalVideo& video) {
    const std::filesystem::path file = snapshot_file(eval_root, video.id);
    Result<std::string> text = read_file(file);
    if (!text.has_value() && text.error().code() == Errc::not_found) {
        return std::optional<Snapshot>{};
    }
    RR_TRY(text);
    RR_TRY_ASSIGN(const json::Json artifact, json::parse(*text));
    RR_TRY_ASSIGN(pipeline::Source source, pipeline::source_from_json(artifact));
    const std::string hash = json::content_hash(artifact).hex();
    RR_TRY_ASSIGN(const Record record, read_record(directory_of(eval_root, video.id) / kSnapshotToml));
    if (record.hash != hash) {
        return fail(Errc::integrity,
                    std::format("{} is not the snapshot {} says ({}, not {}): fetch it again", file.string(),
                                kSnapshotToml, hash, record.hash));
    }
    RR_TRY(check_pin(video, hash));
    return std::optional<Snapshot>{
        Snapshot{.source = std::move(source), .hash = hash, .fetched = record.fetched}};
}

Result<Snapshot> keep_snapshot(const std::filesystem::path& eval_root, const EvalVideo& video,
                               const json::Json& artifact, std::chrono::system_clock::time_point fetched) {
    RR_TRY_ASSIGN(pipeline::Source source, pipeline::source_from_json(artifact));
    RR_TRY(check_against_set(video, source));
    const std::string hash = json::content_hash(artifact).hex();
    RR_TRY(check_pin(video, hash));
    const std::filesystem::path directory = directory_of(eval_root, video.id);
    if (std::error_code error; !std::filesystem::create_directories(directory, error) && error) {
        return fail(Errc::io, std::format("cannot make '{}': {}", directory.string(), error.message()));
    }
    const std::string when =
        std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(fetched));
    RR_TRY(write_file_atomically(snapshot_file(eval_root, video.id), json::dump_pretty(artifact) + "\n"));
    RR_TRY(write_file_atomically(
        directory / kSnapshotToml,
        std::format(
            "# The snapshot beside this file, as `rreport eval fetch` kept it (rr/eval/snapshot.hpp).\n"
            "hash = {}\nfetched = {}\nurl = {}\n",
            detail::toml_string(hash), detail::toml_string(when), detail::toml_string(video.url))));
    return Snapshot{.source = std::move(source), .hash = hash, .fetched = when};
}

Result<Snapshot> fetch_snapshot(const std::filesystem::path& eval_root, const EvalVideo& video,
                                const pipeline::SourceStage& stage, const std::filesystem::path& work,
                                const std::stop_token& stop, std::chrono::system_clock::time_point now) {
    const library::Job job{.id = std::format("eval-fetch-{}", video.id), .source = video.url};
    pipeline::StageContext context{.job = &job, .work_directory = work, .stop = stop};
    RR_TRY_ASSIGN(context.fingerprint, stage.fingerprint(context));
    RR_TRY_ASSIGN(const pipeline::StageOutput output, stage.run(context));
    return keep_snapshot(eval_root, video, output.artifact, now);
}

}  // namespace rr::eval
