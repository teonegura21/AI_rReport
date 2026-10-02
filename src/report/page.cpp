// A job's article as one web page (rr/report/page.hpp).
#include <rr/report/page.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/time_span.hpp>
#include <rr/fidelity/checks.hpp>
#include <rr/fidelity/finding.hpp>

namespace rr::report {

namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::SentenceId;

// --- Times and links ---------------------------------------------------------------------

// YouTube's ids are eleven of A–Z a–z 0–9 _ -: anything else builds no link.
bool youtube_id(std::string_view id) {
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    };
    return id.size() == 11 && std::ranges::all_of(id, allowed);
}

std::int64_t seconds(Millis at) {
    return std::chrono::duration_cast<std::chrono::seconds>(at).count();
}

// What the page knows of when things were said, and where a moment links.
class Timeline {
public:
    explicit Timeline(const Contents& contents) : contents_(&contents) {
        if (contents.video.has_value() && youtube_id(contents.video->id)) {
            video_ = contents.video->id;
        }
    }

    // The video's id: one of YouTube's form, or none.
    [[nodiscard]] const std::optional<std::string>& video() const { return video_; }

    // When sentence `id` starts; none without a transcript that has it.
    [[nodiscard]] std::optional<Millis> said(SentenceId id) const {
        if (!contents_->transcript.has_value()) {
            return std::nullopt;
        }
        const knowledge::Sentence* sentence = contents_->transcript->find(id);
        return sentence == nullptr ? std::nullopt : std::optional{sentence->span.start()};
    }

    // When atom `id` was first said: its first sentence's start.
    [[nodiscard]] std::optional<Millis> said(AtomId id) const {
        const Atom* atom = contents_->ledger.has_value() ? contents_->ledger->find(id) : nullptr;
        return atom == nullptr ? std::nullopt : said(atom->sentences.front());
    }

    // Where `sentence`, said at `at`, links: the video's moment, or the
    // transcript's row.
    [[nodiscard]] std::string link(Millis at, SentenceId sentence) const {
        if (video_.has_value()) {
            return std::format("https://www.youtube.com/watch?v={}&amp;t={}s", *video_, seconds(at));
        }
        return std::format("#{}", sentence);
    }

private:
    const Contents* contents_;
    std::optional<std::string> video_{};
};

// "1:02", linked to the moment `sentence` starts; nothing when the page
// does not know it.
std::string time_link(const Timeline& timeline, SentenceId sentence) {
    const std::optional<Millis> at = timeline.said(sentence);
    if (!at.has_value()) {
        return {};
    }
    return std::format(R"(<a class="time" href="{}" data-t="{}">{}</a>)", timeline.link(*at, sentence),
                       seconds(*at), format_timestamp(*at));
}

// --- The article --------------------------------------------------------------------------

// A sentence of the article: its words, then its markers as links to its
// atoms; it starts the player at its first atom's moment.
std::string sentence_html(std::string_view markdown, const knowledge::ParsedArticle& parsed,
                          const knowledge::CitingSentence& sentence, const Timeline& timeline) {
    const std::string words = escaped(knowledge::without_markers(markdown, parsed, sentence.span));
    if (sentence.markers.empty()) {
        return std::format(R"(<span class="s">{}</span>)", words);
    }
    std::string cites;
    std::string atoms;
    for (const knowledge::Marker& marker : sentence.markers) {
        cites += std::format(R"(<a class="cite" href="#{0}">{0}</a>)", marker.atom);
        atoms += std::format("{}{}", atoms.empty() ? "" : " ", marker.atom);
    }
    const std::optional<Millis> at = timeline.said(sentence.markers.front().atom);
    const std::string moment = at.has_value() ? std::format(R"( data-t="{}")", seconds(*at)) : std::string{};
    return std::format(R"(<span class="s" data-atoms="{}"{}>{}{}</span>)", atoms, moment, words, cites);
}

// The article: each heading with its section's start (the write stage heads
// each section of the plan that has paragraphs, in order), each paragraph
// its sentences.
std::string article_html(const Contents& contents, const Timeline& timeline) {
    if (!contents.article.has_value()) {
        return R"(<p class="missing">The article is not written yet: the job has not reached the write )"
               R"(stage.</p>)";
    }
    const std::string& markdown = contents.article->markdown;
    if (markdown.empty()) {
        return R"(<p class="missing">The video says nothing to retell: every sentence is filler, below.</p>)";
    }
    std::vector<SentenceId> starts;
    if (contents.plan.has_value()) {
        for (const knowledge::SectionPlan& section : contents.plan->sections) {
            if (!section.paragraphs.empty()) {
                starts.push_back(section.first);
            }
        }
    }
    const knowledge::ParsedArticle parsed = knowledge::parse_article(markdown);
    std::string html;
    std::size_t section = 0;
    for (const knowledge::Block& block : parsed.blocks) {
        if (block.kind == knowledge::BlockKind::heading) {
            ++section;
            std::string heading = knowledge::without_markers(markdown, parsed, block.span);
            heading.erase(0, heading.find_first_not_of("# "));
            const std::string start =
                section <= starts.size() ? time_link(timeline, starts[section - 1]) : "";
            html += std::format("<h2 id=\"section-{}\">{} {}</h2>\n", section, escaped(heading), start);
            continue;
        }
        std::string sentences;
        for (const knowledge::CitingSentence& sentence : block.sentences) {
            sentences += (sentences.empty() ? "" : " ") + sentence_html(markdown, parsed, sentence, timeline);
        }
        html += std::format("<p>{}</p>\n", sentences);
    }
    return html;
}

// --- The Fidelity Report -------------------------------------------------------------------

std::string percent(std::size_t part, std::size_t whole) {
    if (whole == 0) {
        return "—";
    }
    return std::format("{:.1f}%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

// A row of the coverage: what, its share, its count, and M1's target.
std::string coverage_row(std::string_view what, std::size_t part, std::size_t whole,
                         std::string_view target) {
    return std::format("<tr><td>{}</td><td>{}</td><td>{} of {}</td><td>{}</td></tr>", what,
                       percent(part, whole), part, whole, target);
}

// What was kept: Ledger A, and where there is an article Ledger B, checked again.
std::string coverage_html(const Contents& contents) {
    const std::optional<fidelity::Report> report = checked(contents);
    if (!report.has_value()) {
        return R"(<p class="missing">Nothing yet: the job has not written Ledger A.</p>)";
    }
    const fidelity::Coverage& kept = report->coverage;
    std::string rows =
        coverage_row("Sentences accounted for", kept.sentences_accounted, kept.sentences, "all");
    if (contents.article.has_value()) {
        rows += coverage_row("Atoms told", kept.atoms_told, kept.atoms, "97% or more");
        rows += std::format("<tr><td>Invalid markers</td><td>{}</td><td>of {} written</td><td>none</td></tr>",
                            kept.markers_invalid, kept.markers);
        rows += coverage_row("Words to quote, kept", kept.must_quotes_kept, kept.must_quotes, "all");
        rows += coverage_row("Numbers kept", kept.numbers_kept, kept.numbers, "99% or more");
    }
    return std::format(
        "<table class=\"coverage\"><tr><th>What</th><th>Kept</th><th></th><th>M1's "
        "target</th></tr>{}</table>\n",
        rows);
}

// What is not told, and why: every filler sentence, every duplicate.
std::string left_out_html(const Contents& contents, const Timeline& timeline) {
    std::string html;
    if (contents.ledger.has_value() && !contents.ledger->fillers.empty()) {
        html += "<h4>Filler: said, and holding nothing to retell</h4>\n<table class=\"fillers\">";
        for (const knowledge::Filler& filler : contents.ledger->fillers) {
            const knowledge::Sentence* sentence =
                contents.transcript.has_value() ? contents.transcript->find(filler.sentence) : nullptr;
            html += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                                time_link(timeline, filler.sentence), knowledge::to_string(filler.label),
                                sentence == nullptr ? std::string{} : escaped(sentence->text));
        }
        html += "</table>\n";
    }
    if (contents.plan.has_value() && !contents.plan->duplicates.empty()) {
        html += "<h4>Said again: told once</h4>\n<table class=\"duplicates\">";
        for (const knowledge::Duplicate& duplicate : contents.plan->duplicates) {
            html +=
                std::format(R"(<tr><td><a href="#{0}">{0}</a></td><td>says again <a href="#{1}">{1}</a></td>)"
                            R"(<td>{2}</td></tr>)",
                            duplicate.atom, duplicate.of, escaped(duplicate.because.value_or("")));
        }
        html += "</table>\n";
    }
    return html.empty() ? std::string{"<p>Nothing: every sentence is told.</p>\n"} : html;
}

// What the stages could not do, stage by stage.
std::string findings_html(const Contents& contents) {
    std::string html;
    for (const StageFindings& stage : contents.findings) {
        if (stage.findings.empty()) {
            continue;
        }
        std::string items;
        for (const std::string& finding : stage.findings) {
            items += std::format("<li>{}</li>", escaped(finding));
        }
        html += std::format("<h4>{}</h4>\n<ul>{}</ul>\n", escaped(stage.stage), items);
    }
    return html.empty() ? std::string{"<p>Nothing: every stage did all it was asked.</p>\n"} : html;
}

// --- The ledger and the transcript --------------------------------------------------------

// "opinion, hedged, aside": an atom's kind, then its labels other than the usual.
std::string labels_of(const Atom& atom) {
    std::string labels{knowledge::to_string(atom.kind)};
    const auto label = [&](std::string_view more, std::string_view usual) {
        if (more != usual && more != knowledge::to_string(atom.kind)) {
            labels += std::format(", {}", more);
        }
    };
    label(knowledge::to_string(atom.modality), knowledge::to_string(knowledge::Modality::asserted));
    label(knowledge::to_string(atom.register_kind), knowledge::to_string(knowledge::Register::main));
    label(knowledge::to_string(atom.intensity), knowledge::to_string(knowledge::Intensity::medium));
    return labels;
}

std::string ledger_html(const Contents& contents, const Timeline& timeline) {
    if (!contents.ledger.has_value()) {
        return {};
    }
    std::string rows;
    for (const Atom& atom : contents.ledger->atoms) {
        const std::string who =
            atom.speaker.has_value() ? std::format(" <em>({})</em>", escaped(*atom.speaker)) : "";
        rows += std::format(R"(<tr id="{0}"><td>{0}</td><td>{1}</td><td>{2}</td><td>{3}{4}</td></tr>)",
                            atom.id, time_link(timeline, atom.sentences.front()), escaped(labels_of(atom)),
                            escaped(atom.text), who);
    }
    return std::format(
        "<details><summary>The ledger: {} atoms</summary>\n<table class=\"ledger\">{}</table>"
        "</details>\n",
        contents.ledger->atoms.size(), rows);
}

std::string transcript_html(const Contents& contents, const Timeline& timeline) {
    if (!contents.transcript.has_value()) {
        return {};
    }
    std::string rows;
    for (const knowledge::Sentence& sentence : contents.transcript->sentences) {
        rows += std::format(R"(<tr id="{0}"><td>{0}</td><td>{1}</td><td>{2}</td><td>{3}</td></tr>)",
                            sentence.id, time_link(timeline, sentence.id),
                            escaped(sentence.speaker.value_or("")), escaped(sentence.text));
    }
    return std::format(
        "<details><summary>The transcript: {} sentences</summary>\n<table "
        "class=\"transcript\">{}</table></details>\n",
        contents.transcript->sentences.size(), rows);
}

// --- The page ------------------------------------------------------------------------------

// The video, the job, and the player.
std::string header_html(const Contents& contents, const Timeline& timeline) {
    std::string about;
    if (contents.video.has_value()) {
        const media::Video& video = *contents.video;
        if (!video.channel.empty()) {
            about += escaped(video.channel) + " · ";
        }
        if (video.duration.has_value()) {
            about += format_timestamp(*video.duration) + " · ";
        }
    }
    if (timeline.video().has_value()) {
        about += std::format("<a href=\"https://www.youtube.com/watch?v={}\">the video</a> \u00b7 ",
                             *timeline.video());
    }
    std::string html = std::format("<p class=\"meta\">{}job {}, tier {}</p>\n", about, escaped(contents.job),
                                   escaped(contents.tier));
    if (timeline.video().has_value()) {
        html +=
            std::format(R"(<iframe id="player" data-src="https://www.youtube-nocookie.com/embed/{0}?rel=0" )"
                        R"(src="https://www.youtube-nocookie.com/embed/{0}?rel=0" title="The video" )"
                        R"(allow="autoplay; encrypted-media" allowfullscreen></iframe>)"
                        "\n",
                        *timeline.video());
    }
    return html;
}

// Each section's heading and start, linked to it.
std::string contents_html(const Contents& contents, const Timeline& timeline) {
    if (!contents.plan.has_value() || !contents.article.has_value() || contents.article->markdown.empty()) {
        return {};
    }
    std::string items;
    std::size_t number = 0;
    for (const knowledge::SectionPlan& section : contents.plan->sections) {
        if (!section.paragraphs.empty()) {
            items += std::format(R"(<li><a href="#section-{}">{}</a> {}</li>)", ++number,
                                 escaped(section.heading), time_link(timeline, section.first));
        }
    }
    return std::format("<nav><ol>{}</ol></nav>\n", items);
}

constexpr std::string_view kStyle = R"(<style>
:root { --ink: #1d1d1f; --page: #fbfbf8; --soft: #6b6b70; --mark: #2a5db0; }
@media (prefers-color-scheme: dark) { :root { --ink: #e8e8e3; --page: #141416; --soft: #a0a0a6; --mark: #8fb3ff; } }
body { margin: 0 auto; max-width: 46rem; padding: 2rem 1rem; background: var(--page); color: var(--ink);
       font: 1.06rem/1.65 Georgia, serif; }
h1, h2, h3, h4, summary, nav, table, .meta { font-family: system-ui, sans-serif; }
a { color: var(--mark); }
a.time { font-size: .75rem; margin-left: .4rem; text-decoration: none; }
.s[data-t] { cursor: pointer; }
.s[data-t]:hover { background: color-mix(in srgb, var(--mark) 12%, transparent); }
a.cite { font: .62rem system-ui, sans-serif; vertical-align: super; margin-left: .1rem; text-decoration: none; }
body.no-cites a.cite { display: none; }
table { border-collapse: collapse; font-size: .85rem; width: 100%; }
td, th { border-bottom: 1px solid color-mix(in srgb, var(--soft) 30%, transparent); padding: .3rem .4rem;
         text-align: left; vertical-align: top; }
.missing, .meta { color: var(--soft); }
iframe { width: 100%; aspect-ratio: 16 / 9; border: 0; }
</style>)";

// A sentence clicked starts the player at its moment; the switch shows or
// hides the markers.
constexpr std::string_view kScript = R"(<script>
document.querySelectorAll('.s[data-t]').forEach(s => s.addEventListener('click', () => {
  const player = document.getElementById('player');
  if (player) { player.src = player.dataset.src + '&start=' + s.dataset.t + '&autoplay=1'; player.scrollIntoView(); }
}));
document.getElementById('cites').addEventListener('change',
  e => document.body.classList.toggle('no-cites', !e.target.checked));
</script>)";

}  // namespace

std::optional<fidelity::Report> checked(const Contents& contents) {
    if (!contents.transcript.has_value() || !contents.ledger.has_value()) {
        return std::nullopt;
    }
    std::vector<std::string> lexicon;
    if (contents.outline.has_value()) {
        for (const knowledge::LexiconTerm& term : contents.outline->brief.lexicon) {
            lexicon.push_back(term.term);
        }
    }
    fidelity::Report report = fidelity::check_ledger(*contents.transcript, *contents.ledger, lexicon);
    if (contents.article.has_value()) {
        const fidelity::ArticleScope scope{.duplicates = contents.plan.has_value()
                                                             ? contents.plan->duplicates
                                                             : std::vector<knowledge::Duplicate>{}};
        report.add(fidelity::check_article(*contents.transcript, *contents.ledger, contents.article->markdown,
                                           scope));
    }
    return report;
}

std::string escaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

std::string render_page(const Contents& contents) {
    const Timeline timeline{contents};
    const std::string title = contents.video.has_value() && !contents.video->title.empty()
                                  ? contents.video->title
                                  : std::string{"A video without a title"};
    std::string html = std::format(
        "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>{0}</title>\n{1}\n</head>\n<body>\n<h1>{0}</h1>\n",
        escaped(title), kStyle);
    html += header_html(contents, timeline);
    html += contents_html(contents, timeline);
    html +=
        "<label class=\"meta\"><input type=\"checkbox\" id=\"cites\" checked> Show which atoms each sentence "
        "tells</label>\n";
    html += "<main>\n" + article_html(contents, timeline) + "</main>\n";
    html += "<section id=\"fidelity\">\n<h2>Fidelity Report</h2>\n";
    html += "<h3>What was kept</h3>\n" + coverage_html(contents);
    html += "<h3>What is not told, and why</h3>\n" + left_out_html(contents, timeline);
    html += "<h3>What the stages could not do</h3>\n" + findings_html(contents);
    html += "</section>\n";
    html += ledger_html(contents, timeline);
    html += transcript_html(contents, timeline);
    html += std::string{kScript} + "\n</body></html>\n";
    return html;
}

}  // namespace rr::report
