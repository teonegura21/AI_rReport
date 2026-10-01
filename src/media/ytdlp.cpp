// yt-dlp, run safely (rr/media/ytdlp.hpp).
#include <rr/media/ytdlp.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <rr/core/file_io.hpp>
#include <rr/json/json.hpp>
#include <rr/proc/process.hpp>

namespace rr::media {

namespace {

// What a failure of yt-dlp means, told by what it says (lowercased).
struct Known {
    std::string_view says;
    Errc code;
    std::string_view meaning;
};

// In order: the first that matches is taken.
constexpr auto kKnown = std::to_array<Known>({
    {.says = "sign in to confirm",
     .code = Errc::needs_user,
     .meaning =
         "YouTube asks to sign in (a check for bots or of age): see `rreport doctor`; cookies are opt-in"},
    {.says = "members-only", .code = Errc::needs_user, .meaning = "the video is for the channel's members"},
    {.says = "http error 5", .code = Errc::unavailable, .meaning = "YouTube is failing for now"},
    {.says = "private video", .code = Errc::not_found, .meaning = "the video is private"},
    {.says = "unavailable", .code = Errc::not_found, .meaning = "the video is unavailable"},
    {.says = "has been removed", .code = Errc::not_found, .meaning = "the video has been removed"},
    {.says = "does not exist", .code = Errc::not_found, .meaning = "the video does not exist"},
    {.says = "unsupported url",
     .code = Errc::invalid_argument,
     .meaning = "yt-dlp reads no video at the address"},
    {.says = "http error 429", .code = Errc::rate_limited, .meaning = "YouTube asks to slow down"},
    {.says = "too many requests", .code = Errc::rate_limited, .meaning = "YouTube asks to slow down"},
    {.says = "timed out", .code = Errc::network, .meaning = "the network timed out"},
    {.says = "unable to download", .code = Errc::network, .meaning = "the network failed"},
    {.says = "urlopen error", .code = Errc::network, .meaning = "the network failed"},
    {.says = "connection", .code = Errc::network, .meaning = "the network failed"},
});

std::string lowercase(std::string_view text) {
    std::string lower{text};
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return lower;
}

// What yt-dlp said last, for a message: its last error line, or the end of
// what it wrote.
std::string last_words(const proc::RunResult& ran) {
    const std::string_view text = ran.stderr_text;
    std::string_view last;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = std::min(text.find('\n', begin), text.size());
        if (const std::string_view line = text.substr(begin, end - begin); line.starts_with("ERROR:")) {
            last = line;
        }
        begin = end + 1;
    }
    return std::string{last.empty() ? ran.stderr_tail(300) : last};
}

Error failure(const proc::RunResult& ran) {
    const std::string said = last_words(ran);
    const std::string lower = lowercase(said);
    const auto* const known =
        std::ranges::find_if(kKnown, [&](const Known& candidate) { return lower.contains(candidate.says); });
    if (known == kKnown.end()) {
        return Error{Errc::unavailable, std::format("yt-dlp failed ({}): {}", ran.status.describe(), said)};
    }
    return Error{known->code, std::format("{}: {}", known->meaning, said)};
}

// Runs yt-dlp with `arguments`, the URL last, after "--"; what it wrote,
// when it succeeded.
Result<proc::RunResult> run(const YtDlpOptions& options, std::vector<std::string> arguments,
                            std::string_view url, const std::stop_token& stop) {
    proc::Command command{.argv = {options.executable}};
    for (std::string& argument : arguments) {
        command.argv.push_back(std::move(argument));
    }
    command.argv.emplace_back("--");
    command.argv.emplace_back(url);
    Result<proc::RunResult> ran = proc::run(command, proc::RunOptions{.timeout = options.timeout}, stop);
    if (!ran.has_value()) {
        if (ran.error().code() == Errc::not_found) {
            return fail(Errc::needs_user, std::format("{} is not installed: python3 -m pip install -U yt-dlp",
                                                      options.executable));
        }
        return fail(std::move(ran).error());
    }
    if (!ran->status.success()) {
        return fail(failure(*ran));
    }
    return ran;
}

Result<void> check_address(std::string_view url) {
    if (url.starts_with("https://") || url.starts_with("http://")) {
        return ok();
    }
    return fail(Errc::invalid_argument, std::format("\"{}\" is no web address (https://\u2026)", url));
}

}  // namespace

Result<Video> YtDlp::video(std::string_view url, const std::stop_token& stop) const {
    RR_TRY(check_address(url));
    RR_TRY_ASSIGN(const proc::RunResult ran, run(options_,
                                                 {"--dump-single-json", "--no-playlist", "--skip-download",
                                                  "--ignore-no-formats-error", "--no-warnings"},
                                                 url, stop));
    if (ran.stdout_truncated) {
        return fail(Errc::invalid_argument, "yt-dlp described the video at more length than rReport reads");
    }
    Result<json::Json> info = json::parse(ran.stdout_text);
    if (!info.has_value()) {
        return fail(Errc::unavailable,
                    std::format("yt-dlp's description is no JSON: {}", info.error().message()));
    }
    return video_from_ytdlp(*info);
}

Result<Captions> YtDlp::captions(std::string_view url, const CaptionChoice& choice,
                                 const std::filesystem::path& directory, const std::stop_token& stop) const {
    RR_TRY(check_address(url));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return fail(Errc::io, std::format("cannot create {}: {}", directory.string(), error.message()));
    }
    const std::string format{transcript::to_string(choice.format)};
    // yt-dlp names the file after the output template, the language and the
    // format: "captions.en-orig.json3". One left by an earlier try is not read.
    const std::filesystem::path file = directory / std::format("captions.{}.{}", choice.language, format);
    std::filesystem::remove(file, error);
    RR_TRY(run(options_,
               {"--skip-download", "--no-playlist", "--ignore-no-formats-error", "--no-warnings",
                choice.kind == CaptionKind::manual ? "--write-subs" : "--write-auto-subs", "--sub-langs",
                choice.language, "--sub-format", format, "--paths", directory.string(), "--output",
                "captions.%(ext)s"},
               url, stop));
    if (!std::filesystem::exists(file, error)) {
        return fail(Errc::not_found, std::format("yt-dlp wrote no captions {} ({}) in {}", choice.language,
                                                 to_string(choice.kind), format));
    }
    RR_TRY_ASSIGN(std::string content, read_file(file));
    return Captions{.content = std::move(content), .format = choice.format};
}

}  // namespace rr::media
