#include <rr/net/sse.hpp>

#include <algorithm>
#include <charconv>
#include <format>

namespace rr::net {

namespace {

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

}  // namespace

Result<void> SseParser::feed(std::string_view bytes, const EventCallback& on_event) {
    if (at_stream_start_ && !bytes.empty()) {
        // The BOM may itself be split across chunks; wait until we can tell.
        const std::size_t available = std::min(bytes.size(), kByteOrderMark.size() - line_.size());
        const std::string candidate = line_ + std::string{bytes.substr(0, available)};
        if (kByteOrderMark.starts_with(candidate)) {
            if (candidate.size() < kByteOrderMark.size()) {
                line_ = candidate;  // still ambiguous
                return ok();
            }
            line_.clear();
            bytes.remove_prefix(available);
        }
        at_stream_start_ = false;
    }

    while (!bytes.empty()) {
        if (skip_line_feed_) {
            skip_line_feed_ = false;
            if (bytes.front() == '\n') {
                bytes.remove_prefix(1);  // the LF of a CRLF split across two chunks
                continue;
            }
        }
        const std::size_t end = bytes.find_first_of("\r\n");
        if (end == std::string_view::npos) {
            if (line_.size() + bytes.size() > max_event_bytes_) {
                return fail(Errc::invalid_argument,
                            std::format("SSE line exceeds {} bytes without a line break", max_event_bytes_));
            }
            line_.append(bytes);
            return ok();
        }
        line_.append(bytes.substr(0, end));
        if (bytes[end] == '\r') {
            skip_line_feed_ = true;  // an LF may follow, here or in the next chunk
        }
        bytes.remove_prefix(end + 1);
        const std::string line = std::move(line_);
        line_.clear();
        RR_TRY(process_line(line, on_event));
    }
    return ok();
}

Result<void> SseParser::process_line(std::string_view line, const EventCallback& on_event) {
    if (line.empty()) {
        return dispatch(on_event);
    }
    if (line.front() == ':') {
        return ok();  // a comment (keep-alive)
    }
    const std::size_t colon = line.find(':');
    const std::string_view field = line.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
    if (value.starts_with(' ')) {
        value.remove_prefix(1);
    }

    if (field == "data") {
        if (data_.size() + value.size() + 1 > max_event_bytes_) {
            return fail(Errc::invalid_argument, std::format("SSE event exceeds {} bytes", max_event_bytes_));
        }
        data_.append(value);
        data_.push_back('\n');
    } else if (field == "event") {
        type_.assign(value);
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) {
            last_event_id_.assign(value);
        }
    } else if (field == "retry") {
        // Only a value made entirely of ASCII digits counts; anything else is ignored.
        const bool digits =
            !value.empty() && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
        std::int64_t milliseconds = 0;
        if (digits &&
            std::from_chars(value.data(), value.data() + value.size(), milliseconds).ec == std::errc{}) {
            retry_ = std::chrono::milliseconds{milliseconds};
        }
    }
    // Any other field is ignored, as the specification requires.
    return ok();
}

Result<void> SseParser::dispatch(const EventCallback& on_event) {
    if (data_.empty()) {
        type_.clear();  // nothing to deliver: an event needs data
        return ok();
    }
    data_.pop_back();  // the '\n' appended after the last data line
    const SseEvent event{.type = type_.empty() ? std::string{"message"} : std::move(type_),
                         .data = std::move(data_),
                         .id = last_event_id_};
    data_.clear();
    type_.clear();
    if (on_event) {
        return on_event(event);
    }
    return ok();
}

std::string format_sse_event(std::string_view data, std::string_view type, std::string_view id) {
    std::string wire;
    wire.reserve(data.size() + type.size() + id.size() + 32);
    if (!type.empty()) {
        wire.append("event: ").append(type).push_back('\n');
    }
    if (!id.empty()) {
        wire.append("id: ").append(id).push_back('\n');
    }
    // Each line of the payload becomes its own `data:` line; the parser joins
    // them again with '\n'. CR and CRLF are normalized to '\n' on the way.
    std::size_t start = 0;
    while (true) {
        const std::size_t end = data.find_first_of("\r\n", start);
        wire.append("data: ").append(data.substr(start, end - start)).push_back('\n');
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
        if (data[end] == '\r' && start < data.size() && data[start] == '\n') {
            ++start;
        }
    }
    wire.push_back('\n');
    return wire;
}

std::string format_sse_comment(std::string_view text) {
    std::string wire = ": ";
    for (const char c : text) {
        wire.push_back(c == '\n' || c == '\r' ? ' ' : c);  // a comment must stay on one line
    }
    wire.append("\n\n");
    return wire;
}

}  // namespace rr::net
