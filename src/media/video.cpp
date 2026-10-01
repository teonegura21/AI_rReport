// A video as yt-dlp describes it (rr/media/video.hpp).
#include <rr/media/video.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rr::media {

namespace {

using transcript::CaptionFormat;

// The text at `object[key]`, when it is a non-empty string.
std::optional<std::string> text_at(const json::Json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

// The seconds at `object[key]` as milliseconds, when they are a time of a
// video: no video lasts years, and a longer one could not be counted.
std::optional<Millis> seconds_at(const json::Json& object, std::string_view key) {
    constexpr double kLongest = 1e8;
    constexpr double kMillisPerSecond = 1000;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) {
        return std::nullopt;
    }
    const double seconds = found->get<double>();
    const bool time = seconds >= 0 && seconds <= kLongest;
    if (!time) {
        return std::nullopt;
    }
    return Millis{std::llround(seconds * kMillisPerSecond)};
}

// "2009-10-25" for yt-dlp's "20091025".
std::optional<std::string> date_at(const json::Json& object, std::string_view key) {
    constexpr std::size_t kDigits = 8;
    const std::optional<std::string> written = text_at(object, key);
    if (!written.has_value() || written->size() != kDigits ||
        written->find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    return std::format("{}-{}-{}", written->substr(0, 4), written->substr(4, 2), written->substr(6, 2));
}

std::vector<Chapter> chapters_at(const json::Json& object) {
    std::vector<Chapter> chapters;
    const auto found = object.find("chapters");
    if (found == object.end() || !found->is_array()) {
        return chapters;
    }
    for (const json::Json& chapter : *found) {
        if (!chapter.is_object()) {
            continue;
        }
        const std::optional<Millis> start = seconds_at(chapter, "start_time");
        const std::optional<Millis> end = seconds_at(chapter, "end_time");
        if (!start.has_value() || !end.has_value()) {
            continue;
        }
        Result<TimeSpan> span = TimeSpan::make(*start, *end);
        if (span.has_value()) {
            chapters.push_back(Chapter{.title = text_at(chapter, "title").value_or(""), .span = *span});
        }
    }
    return chapters;
}

// The tracks of `object[key]`: language → [{"ext": "json3", "url": …}, …].
std::vector<CaptionOffer> offers_at(const json::Json& object, std::string_view key, CaptionKind kind) {
    std::vector<CaptionOffer> offers;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_object()) {
        return offers;
    }
    for (const auto& [language, formats] : found->items()) {
        if (language == "live_chat" || !formats.is_array()) {
            continue;  // a live stream's chat replay, not captions
        }
        CaptionOffer offer{.language = language, .kind = kind};
        for (const json::Json& format : formats) {
            if (std::optional<std::string> extension =
                    format.is_object() ? text_at(format, "ext") : std::nullopt) {
                offer.formats.push_back(*std::move(extension));
            }
        }
        offers.push_back(std::move(offer));
    }
    return offers;
}

// "en" of "en-GB", and of "en-orig".
std::string_view primary(std::string_view language) {
    return language.substr(0, language.find('-'));
}

// The format rReport reads `offer` in, if it reads one of its formats.
std::optional<CaptionFormat> readable(const CaptionOffer& offer) {
    for (const CaptionFormat format : {CaptionFormat::json3, CaptionFormat::vtt, CaptionFormat::srt}) {
        if (std::ranges::contains(offer.formats, transcript::to_string(format))) {
            return format;
        }
    }
    return std::nullopt;
}

// "en (the creator's), en-orig (automatic)": what is on offer, for a message.
std::string offered(const Video& video) {
    std::string list;
    for (const CaptionOffer& offer : video.captions) {
        list += std::format("{}{} ({})", list.empty() ? "" : ", ", offer.language,
                            offer.kind == CaptionKind::manual ? "the creator's" : "automatic");
    }
    return list.empty() ? std::string{"none"} : list;
}

}  // namespace

std::string_view to_string(CaptionKind kind) noexcept {
    return kind == CaptionKind::manual ? "manual" : "automatic";
}

Result<Video> video_from_ytdlp(const json::Json& info) {
    if (!info.is_object()) {
        return fail(Errc::invalid_argument, "yt-dlp described no video");
    }
    if (const std::optional<std::string> type = text_at(info, "_type");
        type.has_value() && *type != "video") {
        return fail(Errc::invalid_argument,
                    std::format("the address is a {}, not a video: give the address of one video", *type));
    }
    const std::optional<std::string> id = text_at(info, "id");
    if (!id.has_value()) {
        return fail(Errc::invalid_argument, "yt-dlp gave the video no ID");
    }
    Video video{.id = *id,
                .url = text_at(info, "webpage_url").value_or(text_at(info, "original_url").value_or("")),
                .title = text_at(info, "title").value_or(""),
                .description = text_at(info, "description").value_or(""),
                .channel = text_at(info, "channel").value_or(text_at(info, "uploader").value_or("")),
                .upload_date = date_at(info, "upload_date"),
                .duration = seconds_at(info, "duration"),
                .language = text_at(info, "language"),
                .license = text_at(info, "license"),
                .chapters = chapters_at(info),
                .captions = offers_at(info, "subtitles", CaptionKind::manual)};
    for (CaptionOffer& offer : offers_at(info, "automatic_captions", CaptionKind::automatic)) {
        video.captions.push_back(std::move(offer));
    }
    if (const auto tags = info.find("tags"); tags != info.end() && tags->is_array()) {
        for (const json::Json& tag : *tags) {
            if (tag.is_string()) {
                video.tags.push_back(tag.get<std::string>());
            }
        }
    }
    const std::optional<std::string> status = text_at(info, "live_status");
    const auto is_live = info.find("is_live");
    video.live = (is_live != info.end() && is_live->is_boolean() && is_live->get<bool>()) ||
                 status == "is_live" || status == "is_upcoming";
    return video;
}

Result<CaptionChoice> choose_captions(const Video& video) {
    if (video.live) {
        return fail(Errc::unavailable, "a live stream has its captions only once it has ended");
    }
    const auto recognized = std::ranges::find_if(video.captions, [](const CaptionOffer& offer) {
        return offer.kind == CaptionKind::automatic && offer.language.ends_with("-orig");
    });
    std::optional<std::string> spoken = video.language;
    if (!spoken.has_value() && recognized != video.captions.end()) {
        spoken =
            recognized->language.substr(0, recognized->language.size() - std::string_view{"-orig"}.size());
    }
    // The tracks that are what was said, best first.
    std::vector<const CaptionOffer*> candidates;
    const auto add = [&](auto&& keep) {
        for (const CaptionOffer& offer : video.captions) {
            if (keep(offer) && !std::ranges::contains(candidates, &offer)) {
                candidates.push_back(&offer);
            }
        }
    };
    if (spoken.has_value()) {
        const auto manual = [](const CaptionOffer& offer) { return offer.kind == CaptionKind::manual; };
        add([&](const CaptionOffer& offer) { return manual(offer) && offer.language == *spoken; });
        add([&](const CaptionOffer& offer) {
            return manual(offer) && primary(offer.language) == primary(*spoken);
        });
        add([&](const CaptionOffer& offer) {
            return !manual(offer) && primary(offer.language) == primary(*spoken) &&
                   offer.language.ends_with("-orig");
        });
        if (recognized == video.captions.end()) {  // an older yt-dlp: the track in the language spoken
            add([&](const CaptionOffer& offer) { return !manual(offer) && offer.language == *spoken; });
        }
    } else if (std::ranges::count(video.captions, CaptionKind::manual, &CaptionOffer::kind) == 1) {
        add([](const CaptionOffer& offer) { return offer.kind == CaptionKind::manual; });
    }
    for (const CaptionOffer* candidate : candidates) {
        if (const std::optional<CaptionFormat> format = readable(*candidate)) {
            return CaptionChoice{.language = candidate->language, .kind = candidate->kind, .format = *format};
        }
    }
    return fail(Errc::not_found,
                std::format("no captions in the language spoken ({}); on offer: {}. The video needs speech "
                            "recognition (milestone M4)",
                            spoken.value_or("not told"), offered(video)));
}

}  // namespace rr::media
