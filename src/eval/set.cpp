// The videos the evaluation measures (rr/eval/set.hpp).
#include <rr/eval/set.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>

#include <rr/core/file_io.hpp>

#include "toml_io.hpp"

namespace rr::eval {

namespace {

constexpr std::array<std::string_view, 4> kTypes{"talk", "conversation", "tutorial", "narrative"};
constexpr std::array<std::string_view, 2> kCaptions{"manual", "automatic"};
constexpr std::array<std::string_view, 2> kSplits{"dev", "test"};
constexpr std::array<std::string_view, 2> kLicences{"cc-by", "us-public-domain"};

// The position of `value` in `names`, which the reader already checked holds it.
template <std::size_t N>
std::size_t index_of(const std::array<std::string_view, N>& names, std::string_view value) {
    return static_cast<std::size_t>(std::ranges::find(names, value) - names.begin());
}

Result<EvalVideo> read_video(detail::Table& table) {
    EvalVideo video;
    RR_TRY_ASSIGN(video.id, table.text("id"));
    if (!valid_video_id(video.id)) {
        return fail(Errc::invalid_argument,
                    std::format("{}: id '{}' must be 1 to 64 of a-z, 0-9 and '-': it names a directory",
                                table.where(), video.id));
    }
    RR_TRY_ASSIGN(video.title, table.text("title"));
    RR_TRY_ASSIGN(video.url, table.text("url"));
    RR_TRY_ASSIGN(const std::string type, table.choice("type", kTypes));
    video.type = static_cast<VideoType>(index_of(kTypes, type));
    RR_TRY_ASSIGN(const std::string captions, table.choice("captions", kCaptions));
    video.captions = captions == "manual" ? media::CaptionKind::manual : media::CaptionKind::automatic;
    RR_TRY_ASSIGN(const std::string split, table.choice("split", kSplits));
    video.split = static_cast<Split>(index_of(kSplits, split));
    RR_TRY_ASSIGN(const std::string licence, table.choice("licence", kLicences));
    video.licence = static_cast<Licence>(index_of(kLicences, licence));
    RR_TRY_ASSIGN(video.attribution, table.text("attribution"));
    RR_TRY_ASSIGN(video.snapshot, table.optional_text("snapshot"));
    return video;
}

}  // namespace

std::string_view to_string(VideoType type) noexcept {
    return kTypes[static_cast<std::size_t>(type)];
}

std::string_view to_string(Split split) noexcept {
    return kSplits[static_cast<std::size_t>(split)];
}

std::string_view to_string(Licence licence) noexcept {
    return kLicences[static_cast<std::size_t>(licence)];
}

const EvalVideo* EvalSet::find(std::string_view id) const noexcept {
    const auto found = std::ranges::find(videos, id, &EvalVideo::id);
    return found == videos.end() ? nullptr : &*found;
}

bool valid_video_id(std::string_view id) noexcept {
    const auto allowed = [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
    };
    return !id.empty() && id.size() <= 64 && id.front() != '-' && std::ranges::all_of(id, allowed);
}

Result<EvalSet> parse_set(std::string_view name, std::string_view toml) {
    const std::string file = std::format("eval/sets/{}.toml", name);
    RR_TRY_ASSIGN(const toml::table document, detail::parse_toml(toml, file));
    for (const auto& [key, value] : document) {
        if (key.str() != "video") {
            return fail(Errc::invalid_argument, std::format("{}: unknown key '{}'", file, key.str()));
        }
    }
    EvalSet set{.name = std::string{name}};
    std::set<std::string, std::less<>> ids;
    RR_TRY(detail::each_table(document, "video", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(EvalVideo video, read_video(table));
        if (!ids.insert(video.id).second) {
            return fail(Errc::invalid_argument,
                        std::format("{}: a second video with the id '{}'", table.where(), video.id));
        }
        set.videos.push_back(std::move(video));
        return ok();
    }));
    return set;
}

Result<EvalSet> read_set(const std::filesystem::path& eval_root, std::string_view name) {
    RR_TRY_ASSIGN(const std::string text, read_file(eval_root / "sets" / std::format("{}.toml", name)));
    return parse_set(name, text);
}

}  // namespace rr::eval
