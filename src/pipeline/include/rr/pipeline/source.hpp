// rr/pipeline/source.hpp — the first stage: what a job reads, read once.
//
// Why this exists
//   A job starts from what the user gave `rreport ingest`: the address of a
//   video, or a caption file on their machine. The source stage reads it,
//   once, into an artifact (schemas/artifacts/source.json): the video as its
//   page describes it (title, description, chapters, licence) and its
//   captions, whole, as downloaded. Every stage after it reads the artifact,
//   never the network: a resumed job reads what the job read, even if the
//   video changed since, and only the first stage can fail for the network.
//
// Where it comes from
//   * A web address (http:// or https://) is read with yt-dlp
//     (rr/media/ytdlp.hpp): the video's description, then the captions
//     chosen in the language spoken (rr/media/video.hpp). A live or upcoming
//     video has no captions yet: the job stops, to be resumed once it ends.
//   * A file named *.source.json is a source read earlier and saved whole:
//     an eval's snapshot of a video (rr/eval/snapshot.hpp). It is checked
//     against the source schema and kept as it is, so the stages after it
//     run as they would on the video it was read from.
//   * Anything else is a caption file. Its name says its format (".vtt",
//     ".srt", ".json3", ".txt": rr/transcript/captions.hpp), and its name
//     without the extension is the title. Nothing else is known about it:
//     who made the captions, in what language.
//
// Its key
//   A web address is read once per library: its key is the address, so a
//   second job for the same address finds the first one's source, and runs
//   no stage again that it need not. Two addresses of one video (youtu.be
//   and youtube.com) are fetched once each, but their artifacts are equal,
//   and so are the keys of every stage after them.
//   A file's key is its name and the hash of its bytes: an edited file is
//   read again, a copy under the same name is not. The bytes the key was made
//   from are the bytes kept: a file that changes while it is read is an
//   error, never a source kept under another source's key.
//
// Thread-safety: immutable; run() may be called from any thread.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/time_span.hpp>
#include <rr/json/json.hpp>
#include <rr/media/video.hpp>
#include <rr/media/ytdlp.hpp>
#include <rr/pipeline/stage.hpp>
#include <rr/transcript/captions.hpp>

namespace rr::pipeline {

enum class Origin : std::uint8_t {
    web,   // a web address, read with yt-dlp
    file,  // a caption file
};

[[nodiscard]] std::string_view to_string(Origin origin) noexcept;

// The captions, as read.
struct SourceCaptions {
    std::optional<std::string> language{};     // none: not known (a file)
    std::optional<media::CaptionKind> kind{};  // none: not known (a file)
    transcript::CaptionFormat format = transcript::CaptionFormat::text;
    std::string content{};

    friend bool operator==(const SourceCaptions&, const SourceCaptions&) = default;
};

// The source artifact.
struct Source {
    Origin origin = Origin::web;
    // From a file: its title, and nothing else. Of the offers, none is kept:
    // the captions read are below. Empty texts are written as null.
    media::Video video{};
    SourceCaptions captions{};

    friend bool operator==(const Source&, const Source&) = default;
};

[[nodiscard]] json::Json to_json(const Source& source);

// Words a minute, the pace untimed captions are counted at: a measured
// speaker's (a podcast's runs nearer 160, a lecture's nearer 140).
inline constexpr std::int64_t kWordsPerMinute = 150;

// How long `source`'s video lasts: the length the site gives; else where
// its captions end, their last word or sound; else, for captions without
// times, their words at kWordsPerMinute. Errors: transcript::read_captions()'s,
// for captions that are not their format.
[[nodiscard]] Result<Millis> length_of(const Source& source);

// Errc::invalid_argument when `document` breaks the schema
// (schemas/artifacts/source.json), or a chapter ends before it starts.
[[nodiscard]] Result<Source> source_from_json(const json::Json& document);

// Whether `source`, as the user gave it, is a web address: it starts with
// "https://" or "http://", in any case.
[[nodiscard]] bool is_web_address(std::string_view source) noexcept;

class SourceStage final : public Stage {
public:
    static constexpr std::string_view kName = "source";

    explicit SourceStage(media::YtDlp ytdlp = media::YtDlp{}) : ytdlp_(std::move(ytdlp)) {}

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override { return {}; }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/source"; }

    // A web address: {"address"}. A file: {"file": its name, "content": the
    // BLAKE3 of its bytes}. Errc::not_found for a file that is not there.
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& context) const override;

    // Errors: yt-dlp's (rr/media/ytdlp.hpp); choose_captions()'s: Errc::not_found
    // for a video without captions in the language spoken, Errc::unavailable
    // for a live or upcoming one; Errc::not_found for a file that is not
    // there; Errc::invalid_argument for a file whose name gives no caption
    // format, or a saved source that is not one; Errc::conflict for a file
    // that changed after its key was made.
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override;

private:
    media::YtDlp ytdlp_;
};

}  // namespace rr::pipeline
