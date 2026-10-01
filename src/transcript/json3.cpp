// json3, YouTube's timed text (formats.hpp).
//
//   {"events": [
//     {"tStartMs": 0, "dDurationMs": 5000, "id": 1, "wpWinPosId": 1},
//     {"tStartMs": 160, "dDurationMs": 4800, "wWinId": 1,
//      "segs": [{"utf8": "hello"}, {"utf8": " everyone", "tOffsetMs": 400}]},
//     {"tStartMs": 2550, "dDurationMs": 2410, "aAppend": 1, "segs": [{"utf8": "\n"}]}]}
//
// An event with segments is a cue, shown from tStartMs for dDurationMs. In
// automatic captions each segment is a word, starting tOffsetMs into it; in a
// creator's, a segment is a run of text. Events without segments place and
// style windows, and an appended "\n" breaks a line: neither holds words.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/json/json.hpp>

#include "formats.hpp"

namespace rr::transcript::formats {

namespace {

// The milliseconds `object[key]`, or `fallback` when the key is absent.
Result<Millis> milliseconds(const json::Json& object, std::string_view key, std::size_t event,
                            std::optional<Millis> fallback = std::nullopt) {
    const auto found = object.find(key);
    if (found == object.end()) {
        if (fallback.has_value()) {
            return *fallback;
        }
        return fail(Errc::invalid_argument, std::format("json3 event {} has no \"{}\"", event, key));
    }
    const bool time = found->is_number() && found->get<double>() >= 0 &&
                      found->get<double>() <= static_cast<double>(kLatest.count());
    if (!time) {
        return fail(Errc::invalid_argument, std::format("json3 event {}: \"{}\" is {}, not milliseconds",
                                                        event, key, json::dump(*found)));
    }
    return Millis{std::llround(found->get<double>())};
}

}  // namespace

Result<std::vector<cues::Cue>> json3(std::string_view content) {
    RR_TRY_ASSIGN(const json::Json document, json::parse(content));
    const bool shaped = document.is_object() && document.contains("events") && document["events"].is_array();
    if (!shaped) {
        return fail(Errc::invalid_argument,
                    "not json3: json3 captions are an object with a list of \"events\"");
    }
    std::vector<cues::Cue> read;
    std::size_t event_number = 0;
    for (const json::Json& event : document["events"]) {
        ++event_number;
        if (!event.is_object() || !event.contains("segs")) {
            continue;  // a window's place or style
        }
        const json::Json& segments = event["segs"];
        if (!segments.is_array()) {
            return fail(Errc::invalid_argument,
                        std::format("json3 event {}: \"segs\" is not a list", event_number));
        }
        RR_TRY_ASSIGN(const Millis start, milliseconds(event, "tStartMs", event_number));
        RR_TRY_ASSIGN(const Millis duration, milliseconds(event, "dDurationMs", event_number, Millis{0}));
        cues::Cue cue{.start = start, .end = start + duration};
        for (const json::Json& segment : segments) {
            const bool text = segment.is_object() && segment.contains("utf8") && segment["utf8"].is_string();
            if (!text) {
                return fail(Errc::invalid_argument,
                            std::format("json3 event {}: a segment without \"utf8\" text", event_number));
            }
            if (segment.contains("tOffsetMs")) {
                RR_TRY_ASSIGN(const Millis offset, milliseconds(segment, "tOffsetMs", event_number));
                cue.marks.push_back(cues::Mark{.offset = cue.text.size(), .time = start + offset});
            }
            cue.text += segment["utf8"].get_ref<const std::string&>();
        }
        read.push_back(std::move(cue));
    }
    return read;
}

}  // namespace rr::transcript::formats
