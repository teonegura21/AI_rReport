#include <rr/fidelity/finding.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace rr::fidelity {

namespace {

constexpr auto kNames = std::to_array<std::string_view>({
    "sentence-accounted",
    "sentence-once",
    "sentence-known",
    "number-said",
    "quote-said",
    "name-said",
    "filler-empty",
    "marker-valid",
    "marker-known",
    "atom-told",
    "number-kept",
    "name-kept",
    "term-kept",
    "must-quote-kept",
    "quote-verbatim",
    "order-kept",
});
static_assert(kNames.size() == static_cast<std::size_t>(Check::order_kept) + 1);

}  // namespace

std::string_view to_string(Check check) noexcept {
    const auto index = static_cast<std::size_t>(check);
    return index < kNames.size() ? kNames[index] : std::string_view{"?"};
}

std::size_t Report::errors() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(
        findings, Severity::error, [](const Finding& finding) { return finding.severity; }));
}

void Report::add(const Report& other) {
    findings.insert(findings.end(), other.findings.begin(), other.findings.end());
    coverage.sentences += other.coverage.sentences;
    coverage.sentences_accounted += other.coverage.sentences_accounted;
    coverage.atoms += other.coverage.atoms;
    coverage.atoms_told += other.coverage.atoms_told;
    coverage.markers += other.coverage.markers;
    coverage.markers_invalid += other.coverage.markers_invalid;
    coverage.must_quotes += other.coverage.must_quotes;
    coverage.must_quotes_kept += other.coverage.must_quotes_kept;
    coverage.numbers += other.coverage.numbers;
    coverage.numbers_kept += other.coverage.numbers_kept;
}

}  // namespace rr::fidelity
