// A provider that answers each request by what it asks, not by the order the
// calls arrive in: a squad's calls arrive in any order.
//
// `respond` sees every request (its mission, model, thinking, messages…) and
// returns the answer or an error. Calls to a model can be held (hold()) until
// released, to see how many the army lets in at once; a held call ends with
// Errc::cancelled when its stop token is triggered, as a real one would.
// Sequences are counted: a request for n answers is n of them.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/llm/provider.hpp>

namespace rr::agents::testing {

class KeyedProvider final : public llm::LlmProvider {
public:
    using Respond = std::function<Result<llm::ChatResponse>(const llm::ChatRequest&)>;

    // What the provider has seen.
    struct State {
        std::vector<llm::ChatRequest> requests{};  // in the order they arrived
        std::map<std::string, int, std::less<>> in_flight{};
        std::map<std::string, int, std::less<>> peak{};  // the most in flight at once
        int answered = 0;                                // calls that returned (held ones not yet)

        [[nodiscard]] int flying(std::string_view model) const {
            const auto found = in_flight.find(model);
            return found == in_flight.end() ? 0 : found->second;
        }
        [[nodiscard]] std::size_t asked(std::string_view mission) const {
            return static_cast<std::size_t>(std::ranges::count_if(
                requests,
                [&](const llm::ChatRequest& request) { return request.tag.mission_id == mission; }));
        }
    };

    explicit KeyedProvider(Respond respond, llm::ProviderCaps caps = {.json_schema = true, .thinking = true})
        : respond_(std::move(respond))
        , caps_(caps) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "keyed"; }
    [[nodiscard]] llm::ProviderCaps caps() const noexcept override { return caps_; }

    [[nodiscard]] Result<llm::ChatResponse> chat(const llm::ChatRequest& request,
                                                 const llm::StreamSink& /*sink*/,
                                                 const std::stop_token& stop) const override {
        const int sequences = request.n.value_or(1);
        {
            std::unique_lock lock(mutex_);
            state_.requests.push_back(request);
            int& flying = state_.in_flight[request.model];
            flying += sequences;
            int& peak = state_.peak[request.model];
            peak = std::max(peak, flying);
            changed_.notify_all();
            if (!changed_.wait(lock, stop, [&] { return !held_.contains(request.model); })) {
                flying -= sequences;
                changed_.notify_all();
                return fail(Errc::cancelled, "the call was stopped while held");
            }
        }
        Result<llm::ChatResponse> response = respond_(request);
        {
            const std::lock_guard lock(mutex_);
            state_.in_flight[request.model] -= sequences;
            ++state_.answered;
            changed_.notify_all();
        }
        return response;
    }

    // Calls to `model` wait until it is released.
    void hold(const std::string& model) {
        const std::lock_guard lock(mutex_);
        held_.insert(model);
    }
    void release(const std::string& model) {
        const std::lock_guard lock(mutex_);
        held_.erase(model);
        changed_.notify_all();
    }

    // Waits up to `timeout` until `predicate` holds of what the provider has
    // seen; whether it does.
    [[nodiscard]] bool wait_until(const std::function<bool(const State&)>& predicate,
                                  std::chrono::milliseconds timeout = std::chrono::seconds{10}) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(state_); });
    }

    [[nodiscard]] State state() const {
        const std::lock_guard lock(mutex_);
        return state_;
    }
    [[nodiscard]] std::vector<llm::ChatRequest> requests_for(std::string_view mission) const {
        const std::lock_guard lock(mutex_);
        std::vector<llm::ChatRequest> found;
        for (const llm::ChatRequest& request : state_.requests) {
            if (request.tag.mission_id == mission) {
                found.push_back(request);
            }
        }
        return found;
    }

private:
    Respond respond_;
    llm::ProviderCaps caps_;
    mutable std::mutex mutex_;
    mutable std::condition_variable_any changed_;
    mutable State state_;
    std::set<std::string, std::less<>> held_;
};

// Releases a held model when it goes out of scope. Declared after the
// threads whose calls the model holds, it is destroyed before they are
// joined: a test that fails halfway still lets them finish, and reports its
// failure instead of hanging.
class Release {
public:
    Release(KeyedProvider& provider, std::string model) : provider_(&provider), model_(std::move(model)) {}
    Release(const Release&) = delete;
    Release& operator=(const Release&) = delete;
    Release(Release&&) = delete;
    Release& operator=(Release&&) = delete;
    ~Release() { provider_->release(model_); }

private:
    KeyedProvider* provider_;
    std::string model_;
};

// The answers to a request: one per choice it asks for, `content(i)` each,
// finished normally, five tokens apiece.
inline llm::ChatResponse reply(const llm::ChatRequest& request,
                               const std::function<std::string(int)>& content) {
    llm::ChatResponse response{.usage = {.prompt_tokens = 20}, .model = request.model};
    for (int choice = 0; choice < request.n.value_or(1); ++choice) {
        response.choices.push_back(llm::Choice{
            .content = content(choice), .finish = llm::FinishReason::stop, .completion_tokens = 5});
        response.usage.completion_tokens += 5;
    }
    return response;
}

// One answer, `content`, that ended as `finish`.
inline llm::ChatResponse reply(const llm::ChatRequest& request, std::string content,
                               llm::FinishReason finish = llm::FinishReason::stop) {
    return llm::ChatResponse{
        .choices = {llm::Choice{.content = std::move(content), .finish = finish, .completion_tokens = 5}},
        .usage = {.prompt_tokens = 20, .completion_tokens = 5},
        .model = request.model};
}

}  // namespace rr::agents::testing
