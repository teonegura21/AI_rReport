#include <rr/net/http.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>

namespace rr::net {

std::string_view to_string(Method method) noexcept {
    switch (method) {
        case Method::get:
            return "GET";
        case Method::head:
            return "HEAD";
        case Method::post:
            return "POST";
        case Method::put:
            return "PUT";
        case Method::patch:
            return "PATCH";
        case Method::del:
            return "DELETE";
    }
    return "GET";
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------
void Headers::add(std::string name, std::string value) {
    fields_.emplace_back(std::move(name), std::move(value));
}

void Headers::set(std::string name, std::string value) {
    remove(name);
    add(std::move(name), std::move(value));
}

void Headers::remove(std::string_view name) {
    std::erase_if(fields_, [&](const Field& field) { return iequals_ascii(field.first, name); });
}

std::optional<std::string_view> Headers::get(std::string_view name) const {
    const auto found =
        std::ranges::find_if(fields_, [&](const Field& field) { return iequals_ascii(field.first, name); });
    if (found == fields_.end()) {
        return std::nullopt;
    }
    return std::string_view{found->second};
}

std::vector<std::string_view> Headers::get_all(std::string_view name) const {
    std::vector<std::string_view> values;
    for (const auto& [field_name, value] : fields_) {
        if (iequals_ascii(field_name, name)) {
            values.emplace_back(value);
        }
    }
    return values;
}

// ---------------------------------------------------------------------------
// Errc <-> HTTP status
// ---------------------------------------------------------------------------
int http_status_for(Errc code) noexcept {
    switch (code) {
        case Errc::invalid_argument:
            return 400;
        case Errc::permission_denied:
            return 403;
        case Errc::not_found:
            return 404;
        case Errc::conflict:
            return 409;
        case Errc::budget_exceeded:
            return 402;
        case Errc::refused:
        case Errc::integrity:
            return 422;  // understood, but the content cannot be accepted
        case Errc::needs_user:
            return 428;  // the user must do something first
        case Errc::rate_limited:
            return 429;
        case Errc::io:
        case Errc::internal:
            return 500;
        case Errc::unsupported:
            return 501;
        case Errc::network:
        case Errc::model_output_invalid:
            return 502;  // an upstream (network peer, model) failed us
        case Errc::unavailable:
        case Errc::cancelled:
            return 503;
        case Errc::timeout:
            return 504;
    }
    return 500;
}

Errc errc_for_http_status(int status) noexcept {
    switch (status) {
        case 401:
        case 403:
            return Errc::permission_denied;
        case 402:
            return Errc::budget_exceeded;
        case 404:
        case 410:
            return Errc::not_found;
        case 408:
            return Errc::timeout;
        case 409:
            return Errc::conflict;
        case 425:
            return Errc::unavailable;
        case 428:
            return Errc::needs_user;
        case 429:
            return Errc::rate_limited;
        case 499:
            return Errc::cancelled;
        case 501:
            return Errc::unsupported;
        case 504:
            return Errc::timeout;
        default:
            break;
    }
    if (status >= 500) {
        // Transient by convention: model APIs answer 500/502/503/529 for
        // overload and recommend retrying.
        return Errc::unavailable;
    }
    return Errc::invalid_argument;
}

// ---------------------------------------------------------------------------
// Error bodies
// ---------------------------------------------------------------------------
std::string error_body(const Error& error) {
    json::Json detail = {{"code", std::string{to_string(error.code())}}, {"message", error.message()}};
    if (!error.context().empty()) {
        json::Json context = json::Json::array();
        for (const std::string& layer : error.context()) {
            context.push_back(layer);
        }
        detail["context"] = std::move(context);
    }
    if (error.retry_after().has_value()) {
        detail["retry_after_ms"] = error.retry_after()->count();
    }
    return json::dump(json::Json{{"error", std::move(detail)}});
}

namespace {

// The start of a body, on one line, for error messages.
std::string snippet(std::string_view body, std::size_t limit = 300) {
    std::string text{body.substr(0, limit)};
    std::ranges::replace(text, '\n', ' ');
    std::ranges::replace(text, '\r', ' ');
    if (body.size() > limit) {
        text += "…";
    }
    return text;
}

// Our own error format, or the common shapes of other APIs:
//   OpenAI-style     {"error": {"message": "...", "type": "...", "code": "..."}}
//   Anthropic-style  {"type": "error", "error": {"type": "...", "message": "..."}}
struct ParsedErrorBody {
    std::optional<Errc> code;  // only for our own format
    std::optional<std::string> message;
    std::vector<std::string> context;
    std::optional<std::chrono::milliseconds> retry_after;
};

ParsedErrorBody parse_error_body(std::string_view body) {
    ParsedErrorBody parsed;
    const auto document = json::parse(body);
    if (!document.has_value() || !document->is_object()) {
        return parsed;
    }
    const auto error = document->find("error");
    if (error == document->end() || !error->is_object()) {
        return parsed;
    }
    if (const auto code = error->find("code"); code != error->end() && code->is_string()) {
        parsed.code = errc_from_string(code->get<std::string>());  // nullopt for foreign codes
    }
    if (const auto message = error->find("message"); message != error->end() && message->is_string()) {
        parsed.message = message->get<std::string>();
    }
    if (const auto context = error->find("context"); context != error->end() && context->is_array()) {
        for (const auto& layer : *context) {
            if (layer.is_string()) {
                parsed.context.push_back(layer.get<std::string>());
            }
        }
    }
    if (const auto retry = error->find("retry_after_ms");
        retry != error->end() && retry->is_number_integer() && retry->get<std::int64_t>() >= 0) {
        parsed.retry_after = std::chrono::milliseconds{retry->get<std::int64_t>()};
    }
    return parsed;
}

}  // namespace

Error error_from_response(int status, const Headers& headers, std::string_view body) {
    ParsedErrorBody parsed = parse_error_body(body);
    Error error = parsed.code.has_value()
                      ? Error{*parsed.code, parsed.message.value_or(std::format("HTTP {}", status))}
                      : Error{errc_for_http_status(status),
                              std::format("HTTP {}: {}", status, parsed.message.value_or(snippet(body)))};
    for (std::string& layer : parsed.context) {
        error = std::move(error).with_context(std::move(layer));
    }
    std::optional<std::chrono::milliseconds> retry_after = parsed.retry_after;
    if (const auto header = headers.get("Retry-After"); header.has_value()) {
        retry_after = parse_retry_after(*header);
    }
    if (retry_after.has_value()) {
        error = std::move(error).with_retry_after(*retry_after);
    }
    return error;
}

std::optional<Error> error_from_event(std::string_view data) {
    const auto document = json::parse(data);
    if (!document.has_value() || !document->is_object()) {
        return std::nullopt;
    }
    const auto error = document->find("error");
    if (error == document->end() || !error->is_object()) {
        return std::nullopt;
    }
    // Foreign shapes carry the HTTP status the error would have had as an
    // integer "code"; without one, the stream failed on the server's side.
    int status = 500;
    if (const auto code = error->find("code"); code != error->end() && code->is_number_integer()) {
        const auto value = code->get<std::int64_t>();
        if (value >= 400 && value <= 599) {
            status = static_cast<int>(value);
        }
    }
    return error_from_response(status, Headers{}, data);
}

// ---------------------------------------------------------------------------
// Retry-After
// ---------------------------------------------------------------------------
namespace {

std::optional<int> parse_int(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// 1 for "Jan" … 12 for "Dec".
std::optional<unsigned> month_number(std::string_view name) {
    static constexpr std::array<std::string_view, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (std::size_t index = 0; index < kMonths.size(); ++index) {
        if (kMonths.at(index) == name) {
            return static_cast<unsigned>(index + 1);
        }
    }
    return std::nullopt;
}

// IMF-fixdate, the only date format RFC 9110 lets senders use:
//   "Sun, 06 Nov 1994 08:49:37 GMT"
std::optional<std::chrono::system_clock::time_point> parse_imf_fixdate(std::string_view text) {
    const std::vector<std::string_view> parts = split(text, ' ');
    if (parts.size() != 6 || !parts[0].ends_with(',') || parts[5] != "GMT" || parts[4].size() != 8) {
        return std::nullopt;
    }
    const auto month = month_number(parts[2]);
    const auto day = parse_int(parts[1]);
    const auto year = parse_int(parts[3]);
    const std::string_view clock = parts[4];
    const auto hours = parse_int(clock.substr(0, 2));
    const auto minutes = parse_int(clock.substr(3, 2));
    const auto seconds = parse_int(clock.substr(6, 2));
    if (!month || !day || !year || !hours || !minutes || !seconds || clock[2] != ':' || clock[5] != ':' ||
        *hours > 23 || *minutes > 59 || *seconds > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{*month},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hours} + std::chrono::minutes{*minutes} +
           std::chrono::seconds{*seconds};
}

}  // namespace

std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view value,
                                                           std::chrono::system_clock::time_point now) {
    const std::string_view text = trim(value);
    if (text.empty()) {
        return std::nullopt;
    }
    if (std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        // Delay-seconds. Clamp absurd values instead of overflowing.
        constexpr std::int64_t kMaxSeconds = std::int64_t{7} * 24 * 3600;
        std::int64_t seconds = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
        if (error == std::errc::result_out_of_range || seconds > kMaxSeconds) {
            seconds = kMaxSeconds;
        } else if (error != std::errc{} || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return std::chrono::milliseconds{seconds * 1000};
    }
    const auto date = parse_imf_fixdate(text);
    if (!date.has_value()) {
        return std::nullopt;
    }
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(*date - now);
    return std::max(delay, std::chrono::milliseconds{0});
}

// ---------------------------------------------------------------------------
// URL encoding
// ---------------------------------------------------------------------------
std::string url_encode(std::string_view component) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(component.size());
    for (const char c : component) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
                                byte == '~';
        if (unreserved) {
            encoded.push_back(c);
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex.at(byte >> 4U));
            encoded.push_back(kHex.at(byte & 0x0FU));
        }
    }
    return encoded;
}

}  // namespace rr::net
