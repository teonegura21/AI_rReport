// rr/oracle/oracle.hpp — a model that answers every mission right, from the
// request alone; and the faults real models make, on the calls a test picks.
//
// Why this exists
//   The pipeline is tested end to end without a GPU (layer L3): every stage,
//   every check, every repair and escalation, on synthetic videos whose right
//   answers are known. Two stand-ins make that possible:
//
//   * Oracle, a provider that knows how to compute the right answer to each
//     kind of mission from the request it is given: the sentences in the
//     packet, the atoms, the plan. It is deterministic, so a pipeline run
//     on it can be compared with goldens. Each stage teaches it its mission
//     kind as the stage lands (plan P17–P20). A kind it was not taught is
//     Errc::unsupported: the oracle never guesses.
//
//   * FaultyProvider, which wraps any provider and makes the mistakes real
//     models make, where a test says. The faults are those the plan lists:
//       - a refusal;
//       - an answer cut short;
//       - broken JSON;
//       - a call that runs out while thinking;
//       - a wrong number;
//       - a citation marker that cites nothing;
//       - a dropped sentence reference;
//       - a server error;
//       - a stage's own edit (softening a quote, misattributing).
//     The rule every fault is tested against: it ends repaired, or published
//     as a finding; never silently in the article.
//
//   * RequestLog, which keeps every request a provider is sent: what a test
//     reads to check what a stage asked, and how.
//
// Thread-safety: chat() is safe from many threads. Teach the oracle before
//   its first call.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/error.hpp>
#include <rr/core/result.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/provider.hpp>

namespace rr::oracle {

// Computes the right answer to a mission from its request.
using Answerer = std::function<Result<std::string>(const llm::ChatRequest& request)>;

class Oracle final : public llm::LlmProvider {
public:
    explicit Oracle(llm::ProviderCaps caps = {.json_schema = true, .thinking = true});

    // Answers the missions of `kind` (their calls' purpose) with `answerer`.
    void teach(std::string kind, Answerer answerer);

    [[nodiscard]] std::string_view name() const noexcept override { return "oracle"; }
    [[nodiscard]] llm::ProviderCaps caps() const noexcept override { return caps_; }

    // The answer, once per choice asked for: an oracle has one right answer.
    // Tokens are counted at four characters each, prompt and answer alike.
    [[nodiscard]] Result<llm::ChatResponse> chat(const llm::ChatRequest& request, const llm::StreamSink& sink,
                                                 const std::stop_token& stop) const override;

private:
    llm::ProviderCaps caps_;
    std::map<std::string, Answerer, std::less<>> answerers_;
};

// The calls a fault hits.
struct When {
    std::string mission{};           // a mission id; empty: any
    std::string kind{};              // a mission kind (the calls' purpose); empty: any
    std::string model{};             // a model; empty: any
    std::optional<bool> thinking{};  // calls that think, or that do not; none: either
    int calls = 1;                   // the first this many matching calls of each mission (0: every one)
};

// A mistake, made on the calls `when` matches.
struct Fault {
    std::string name{};
    When when{};
    // The error the call ends with, instead of an answer.
    std::optional<Errc> error{};
    // What the answer becomes: each choice is changed, and says whether it
    // had something to change. Not used with `error`.
    std::function<bool(llm::Choice&)> change{};
};

// The faults. Each changes every choice of the calls it hits.
[[nodiscard]] Fault refusal(When when);             // "I'm sorry, but I can't help with that."
[[nodiscard]] Fault truncation(When when);          // the first half, ended by the token limit
[[nodiscard]] Fault broken_json(When when);         // the last closing brace or bracket gone
[[nodiscard]] Fault out_while_thinking(When when);  // no answer: the limit came while thinking
[[nodiscard]] Fault wrong_number(When when);        // the first number in the answer, plus one
[[nodiscard]] Fault bad_marker(When when);          // the first [A#] marker cites A999, which is no atom
[[nodiscard]] Fault dropped_reference(When when);   // the first "S#" sentence reference left out
[[nodiscard]] Fault server_error(When when, Errc code);
// A stage's own fault: `change` rewrites an answer's text.
[[nodiscard]] Fault edit(std::string name, When when, std::function<std::string(std::string)> change);

// A fault that was made: which, on what.
struct Injection {
    std::string fault{};
    std::string mission{};
    std::string model{};
    bool changed = false;  // false: the answer had nothing the fault could change
};

class FaultyProvider final : public llm::LlmProvider {
public:
    // `inner` must outlive this provider. The first fault that matches a
    // call is the one made. A stream sink hears the answer as `inner` gave
    // it, before the fault.
    FaultyProvider(const llm::LlmProvider& inner, std::vector<Fault> faults);

    [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
    [[nodiscard]] llm::ProviderCaps caps() const noexcept override { return inner_->caps(); }
    [[nodiscard]] Result<llm::ChatResponse> chat(const llm::ChatRequest& request, const llm::StreamSink& sink,
                                                 const std::stop_token& stop) const override;

    // Every fault made so far, in order.
    [[nodiscard]] std::vector<Injection> injected() const;

private:
    const llm::LlmProvider* inner_;
    std::vector<Fault> faults_;
    mutable std::mutex mutex_;                                         // guards the counts and the record
    mutable std::map<std::pair<std::size_t, std::string>, int> hits_;  // (fault, mission) → calls hit
    mutable std::vector<Injection> injected_;
};

// Passes every call on to `inner`, and keeps its request: what a test reads
// to check what a stage asked.
class RequestLog final : public llm::LlmProvider {
public:
    // `inner` must outlive the log.
    explicit RequestLog(const llm::LlmProvider& inner) : inner_(&inner) {}

    [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
    [[nodiscard]] llm::ProviderCaps caps() const noexcept override { return inner_->caps(); }
    [[nodiscard]] Result<llm::ChatResponse> chat(const llm::ChatRequest& request, const llm::StreamSink& sink,
                                                 const std::stop_token& stop) const override;

    // Every request so far, in the order they came.
    [[nodiscard]] std::vector<llm::ChatRequest> requests() const;

private:
    const llm::LlmProvider* inner_;
    mutable std::mutex mutex_;  // guards requests_
    mutable std::vector<llm::ChatRequest> requests_;
};

}  // namespace rr::oracle
