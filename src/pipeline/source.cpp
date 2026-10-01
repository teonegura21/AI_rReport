// The first stage: what a job reads, read once (rr/pipeline/source.hpp).
#include <rr/pipeline/source.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

#include <rr/core/file_io.hpp>
#include <rr/core/hash.hpp>
#include <rr/core/strings.hpp>
#include <rr/knowledge/artifacts.hpp>

namespace rr::pipeline {

namespace {

constexpr std::array kFormats{transcript::CaptionFormat::json3, transcript::CaptionFormat::vtt,
                              transcript::CaptionFormat::srt, transcript::CaptionFormat::text};

// An empty text is not known: written as null.
json::Json text_or_null(std::string_view text) {
    return text.empty() ? json::Json(nullptr) : json::Json(std::string{text});
}

json::Json optional_or_null(const std::optional<std::string>& text) {
    return text_or_null(text.value_or(""));
}

// A member the schema allows to be null, as an optional.
std::optional<std::string> optional_text(const json::Json& object, std::string_view key) {
    const json::Json& value = object.at(key);
    return value.is_null() ? std::nullopt : std::optional<std::string>{value.get<std::string>()};
}

json::Json to_json(const media::Video& video) {
    json::Json chapters = json::Json::array();
    for (const media::Chapter& chapter : video.chapters) {
        chapters.push_back(json::Json{{"title", chapter.title},
                                      {"start_ms", chapter.span.start().count()},
                                      {"end_ms", chapter.span.end().count()}});
    }
    json::Json duration = nullptr;
    if (video.duration.has_value()) {
        duration = video.duration->count();
    }
    return json::Json{{"id", text_or_null(video.id)},
                      {"url", text_or_null(video.url)},
                      {"title", video.title},
                      {"description", video.description},
                      {"channel", text_or_null(video.channel)},
                      {"upload_date", optional_or_null(video.upload_date)},
                      {"duration_ms", std::move(duration)},
                      {"language", optional_or_null(video.language)},
                      {"license", optional_or_null(video.license)},
                      {"tags", video.tags},
                      {"chapters", std::move(chapters)}};
}

Result<media::Video> video_from_json(const json::Json& object) {
    media::Video video{.id = optional_text(object, "id").value_or(""),
                       .url = optional_text(object, "url").value_or(""),
                       .title = object.at("title").get<std::string>(),
                       .description = object.at("description").get<std::string>(),
                       .channel = optional_text(object, "channel").value_or(""),
                       .upload_date = optional_text(object, "upload_date"),
                       .language = optional_text(object, "language"),
                       .license = optional_text(object, "license"),
                       .tags = object.at("tags").get<std::vector<std::string>>()};
    if (const json::Json& duration = object.at("duration_ms"); !duration.is_null()) {
        video.duration = Millis{duration.get<std::int64_t>()};
    }
    for (const json::Json& chapter : object.at("chapters")) {
        RR_TRY_ASSIGN(const TimeSpan span, TimeSpan::make(Millis{chapter.at("start_ms").get<std::int64_t>()},
                                                          Millis{chapter.at("end_ms").get<std::int64_t>()}));
        video.chapters.push_back(
            media::Chapter{.title = chapter.at("title").get<std::string>(), .span = span});
    }
    return video;
}

// The video as the artifact keeps it: what the stages after this one read.
// Empty tags say nothing, and are left out. (So are the offers, which
// to_json() never writes: the captions read are kept whole instead.)
media::Video kept(media::Video video) {
    std::erase(video.tags, std::string{});
    if (video.title.empty()) {
        video.title = video.id;  // the schema needs a title, and a page may give none
    }
    return video;
}

Result<StageOutput> from_web(const media::YtDlp& ytdlp, const StageContext& context,
                             std::string_view address) {
    RR_TRY_ASSIGN(media::Video video, ytdlp.video(address, context.stop));
    RR_TRY_ASSIGN(const media::CaptionChoice choice, media::choose_captions(video));
    if (std::error_code error; !std::filesystem::create_directories(context.work_directory, error) && error) {
        return fail(Errc::io, std::format("cannot make the job's directory '{}': {}",
                                          context.work_directory.string(), error.message()));
    }
    RR_TRY_ASSIGN(media::Captions captions,
                  ytdlp.captions(address, choice, context.work_directory, context.stop));
    const Source source{.origin = Origin::web,
                        .video = kept(std::move(video)),
                        .captions = SourceCaptions{.language = choice.language,
                                                   .kind = choice.kind,
                                                   .format = captions.format,
                                                   .content = std::move(captions.content)}};
    return StageOutput{.artifact = to_json(source)};
}

// A source read earlier and saved whole: an eval's snapshot (rr/eval/snapshot.hpp).
constexpr std::string_view kSavedSource = ".source.json";

Result<StageOutput> from_file(const StageContext& context, const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    const bool saved = name.ends_with(kSavedSource);
    const std::optional<transcript::CaptionFormat> format = transcript::format_of(name);
    if (!saved && !format.has_value()) {
        return fail(
            Errc::invalid_argument,
            std::format("cannot tell the captions' format from the name '{}': name the file .vtt, .srt, "
                        ".json3 or .txt, or *.source.json for a source saved earlier",
                        name));
    }
    RR_TRY_ASSIGN(std::string content, read_file(path));
    if (blake3(content).hex() != context.fingerprint.at("content").get<std::string>()) {
        return fail(Errc::conflict,
                    std::format("'{}' changed while it was being read: ingest it again once it is "
                                "written",
                                path.string()));
    }
    if (saved) {
        // Kept as it is, once checked: its stages run as they would on what it was read from.
        RR_TRY_ASSIGN(json::Json document, json::parse(content));
        RR_TRY(source_from_json(document));
        return StageOutput{.artifact = std::move(document)};
    }
    std::string title = path.stem().string();
    if (title.empty()) {
        title = path.filename().string();
    }
    const Source source{.origin = Origin::file,
                        .video = media::Video{.title = std::move(title)},
                        .captions = SourceCaptions{.format = *format, .content = std::move(content)}};
    return StageOutput{.artifact = to_json(source)};
}

}  // namespace

std::string_view to_string(Origin origin) noexcept {
    return origin == Origin::web ? "web" : "file";
}

json::Json to_json(const Source& source) {
    json::Json kind = "unknown";
    if (source.captions.kind.has_value()) {
        kind = std::string{media::to_string(*source.captions.kind)};
    }
    return json::Json{
        {"origin", std::string{to_string(source.origin)}},
        {"video", to_json(source.video)},
        {"captions", json::Json{{"language", optional_or_null(source.captions.language)},
                                {"kind", std::move(kind)},
                                {"format", std::string{transcript::to_string(source.captions.format)}},
                                {"content", source.captions.content}}}};
}

Result<Source> source_from_json(const json::Json& document) {
    if (Result<void> valid = knowledge::artifact_schemas().validate("artifacts/source", document);
        !valid.has_value()) {
        return fail(Errc::invalid_argument, std::format("not a source: {}", valid.error().message()));
    }
    Source source{.origin = document.at("origin").get<std::string>() == "web" ? Origin::web : Origin::file};
    RR_TRY_ASSIGN(source.video, video_from_json(document.at("video")));
    const json::Json& captions = document.at("captions");
    source.captions.language = optional_text(captions, "language");
    if (const std::string kind = captions.at("kind").get<std::string>(); kind != "unknown") {
        source.captions.kind = kind == "manual" ? media::CaptionKind::manual : media::CaptionKind::automatic;
    }
    const std::string format = captions.at("format").get<std::string>();
    // The schema allows the four formats, and no other.
    source.captions.format = *std::ranges::find(kFormats, format, transcript::to_string);
    source.captions.content = captions.at("content").get<std::string>();
    return source;
}

bool is_web_address(std::string_view source) noexcept {
    const auto starts = [&](std::string_view scheme) {
        return source.size() >= scheme.size() && iequals_ascii(source.substr(0, scheme.size()), scheme);
    };
    return starts("https://") || starts("http://");
}

Result<json::Json> SourceStage::fingerprint(const StageContext& context) const {
    const std::string& source = context.job->source;
    if (is_web_address(source)) {
        return json::Json{{"address", source}};
    }
    const std::filesystem::path path{source};
    RR_TRY_ASSIGN(const std::string content, read_file(path));
    return json::Json{{"file", path.filename().string()}, {"content", blake3(content).hex()}};
}

Result<StageOutput> SourceStage::run(const StageContext& context) const {
    const std::string& source = context.job->source;
    if (is_web_address(source)) {
        return from_web(ytdlp_, context, source);
    }
    return from_file(context, std::filesystem::path{source});
}

}  // namespace rr::pipeline
