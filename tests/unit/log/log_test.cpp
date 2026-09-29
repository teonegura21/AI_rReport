#include <rr/log/log.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace rr::log {
namespace {

// Installs a MemorySink for the duration of a test and restores a quiet
// logger afterwards. The logger is process-wide, so every test starts clean.
class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto sink = std::make_unique<MemorySink>();
        memory_ = sink.get();
        std::vector<std::unique_ptr<Sink>> sinks;
        sinks.push_back(std::move(sink));
        set_sinks(std::move(sinks));
        set_level(Level::trace);
    }

    void TearDown() override {
        set_sinks({});
        set_level(Level::info);
    }

    [[nodiscard]] const std::vector<MemorySink::Entry>& entries() const { return memory_->entries(); }

private:
    MemorySink* memory_ = nullptr;
};

TEST_F(LogTest, FormatsMessagesAndRecordsLevels) {
    info("stage {} took {} ms", "atomize", 42);
    warn("slow");
    ASSERT_EQ(entries().size(), 2U);
    EXPECT_EQ(entries()[0].message, "stage atomize took 42 ms");
    EXPECT_EQ(entries()[0].level, Level::info);
    EXPECT_EQ(entries()[1].level, Level::warn);
}

TEST_F(LogTest, LevelThresholdFiltersRecords) {
    set_level(Level::warn);
    debug("hidden");
    info("hidden too");
    error("shown");
    ASSERT_EQ(entries().size(), 1U);
    EXPECT_EQ(entries()[0].message, "shown");
    EXPECT_FALSE(enabled(Level::info));
    set_level(Level::off);
    error("nothing passes 'off'");
    EXPECT_EQ(entries().size(), 1U);
}

TEST_F(LogTest, ScopedFieldsNestAndUnwind) {
    {
        const ScopedField job{"job", "01J"};
        {
            const ScopedField stage{"stage", "atomize"};
            info("inner");
        }
        info("outer");
    }
    info("none");
    ASSERT_EQ(entries().size(), 3U);
    EXPECT_EQ(entries()[0].fields, (std::vector<Field>{{"job", "01J"}, {"stage", "atomize"}}));
    EXPECT_EQ(entries()[1].fields, (std::vector<Field>{{"job", "01J"}}));
    EXPECT_TRUE(entries()[2].fields.empty());
}

TEST_F(LogTest, ContextTravelsToAnotherThreadWithASnapshot) {
    const ScopedField job{"job", "01J"};
    std::vector<Field> snapshot = context_snapshot();
    std::thread worker([snapshot = std::move(snapshot)]() mutable {
        const ScopedContext context{std::move(snapshot)};
        info("on the pool");
    });
    worker.join();
    ASSERT_EQ(entries().size(), 1U);
    EXPECT_EQ(entries()[0].fields, (std::vector<Field>{{"job", "01J"}}));
}

TEST(Levels, ParseNames) {
    EXPECT_EQ(level_from_string("debug"), Level::debug);
    EXPECT_EQ(level_from_string("warning"), Level::warn);
    EXPECT_FALSE(level_from_string("loud").has_value());
    EXPECT_EQ(to_string(Level::error), "error");
}

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("rr_log_test_" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() { std::filesystem::remove_all(path_); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

TEST(JsonlSink, WritesOneJsonObjectPerLineWithContext) {
    const TempDir dir;
    const auto file = dir.path() / "logs" / "hub.jsonl";
    auto sink = make_jsonl_file_sink(file).value();
    const std::vector<Field> fields{{"job", "01J"}, {"msg", "collides with the envelope"}};
    sink->write(Record{.time = std::chrono::system_clock::now(),
                       .level = Level::info,
                       .message = "hello",
                       .fields = fields});
    sink->flush();

    std::ifstream in(file);
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    const auto parsed = nlohmann::json::parse(line);
    EXPECT_EQ(parsed["msg"], "hello");
    EXPECT_EQ(parsed["level"], "info");
    EXPECT_EQ(parsed["job"], "01J");
    EXPECT_EQ(parsed["ctx_msg"], "collides with the envelope");
    EXPECT_TRUE(parsed["ts"].get<std::string>().ends_with("Z"));
}

TEST(JsonlSink, RotatesWhenTheFileGrowsTooLarge) {
    const TempDir dir;
    const auto file = dir.path() / "rt.jsonl";
    auto sink = make_jsonl_file_sink(file, /*max_bytes=*/200, /*max_files=*/2).value();
    for (int index = 0; index < 20; ++index) {
        sink->write(Record{.time = std::chrono::system_clock::now(),
                           .level = Level::info,
                           .message = "a line of text",
                           .fields = {}});
    }
    sink->flush();
    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_TRUE(std::filesystem::exists(file.string() + ".1"));
    EXPECT_TRUE(std::filesystem::exists(file.string() + ".2"));
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".3"));  // oldest dropped
    EXPECT_LE(std::filesystem::file_size(file), 200U);
}

TEST(JsonlSink, RejectsZeroFiles) {
    EXPECT_EQ(make_jsonl_file_sink("unused.jsonl", 100, 0).error().code(), Errc::invalid_argument);
}

}  // namespace
}  // namespace rr::log
