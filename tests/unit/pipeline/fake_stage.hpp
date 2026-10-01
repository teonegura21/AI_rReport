// A stage a test scripts: what it reads and makes, how it fails, and what it
// was given when it last ran.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/error.hpp>
#include <rr/json/json.hpp>
#include <rr/pipeline/stage.hpp>

namespace rr::pipeline::fakes {

// A transcript of one sentence: an artifact a fake stage can make.
inline json::Json said(std::string_view text) {
    return json::Json{{"sentences",
                       {{{"id", "S1"},
                         {"text", std::string{text}},
                         {"start_ms", 0},
                         {"end_ms", 500},
                         {"speaker", nullptr}}}}};
}

// What a fake stage does, which a test may change between runs.
struct Script {
    int version = 1;
    std::vector<std::string> inputs{};
    json::Json fingerprint = json::Json::object();
    json::Json artifact = said("Hello.");
    std::vector<std::string> findings{};
    std::optional<Error> error{};
    bool samples = false;
    bool reusable = true;
    std::string schema = "artifacts/transcript";
    std::function<void(const StageContext&)> on_run{};             // what else it does as it runs
    int runs = 0;                                                  // how many times it ran
    std::map<std::string, json::Json, std::less<>> inputs_seen{};  // what it read when it last ran
    json::Json fingerprint_seen{};  // what run() was told its key was made from
};

class FakeStage final : public Stage {
public:
    FakeStage(std::string name, std::shared_ptr<Script> script)
        : name_(std::move(name))
        , script_(std::move(script)) {}

    [[nodiscard]] std::string_view name() const override { return name_; }
    [[nodiscard]] int version() const override { return script_->version; }
    [[nodiscard]] std::vector<std::string> inputs() const override { return script_->inputs; }
    [[nodiscard]] std::string_view schema() const override { return script_->schema; }
    [[nodiscard]] bool samples() const override { return script_->samples; }
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& /*context*/) const override {
        return script_->fingerprint;
    }
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override {
        ++script_->runs;
        script_->inputs_seen = context.inputs;
        script_->fingerprint_seen = context.fingerprint;
        if (script_->on_run) {
            script_->on_run(context);
        }
        if (script_->error.has_value()) {
            return std::unexpected(*script_->error);
        }
        return StageOutput{
            .artifact = script_->artifact, .findings = script_->findings, .reusable = script_->reusable};
    }

private:
    std::string name_;
    std::shared_ptr<Script> script_;
};

}  // namespace rr::pipeline::fakes
