// A job's page contents read back from a real library: each stage done, in
// the order the stages ran, as it ran last; the seams stage's article over
// the write stage's; a failed run's nothing; an eval annotation refused.
#include <rr/report/contents.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/core/error.hpp>
#include <rr/core/strings.hpp>
#include <rr/db/database.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/library/library.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/plan_stage.hpp>
#include <rr/pipeline/seams_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/write_stage.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "../pipeline/lecture.hpp"

namespace rr::report {
namespace {

using namespace std::chrono_literals;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testkit::unwrap;

// The article the write stage wrote for the lecture, and the seams stage's,
// a seam smoothed.
std::string written_article() {
    return pipeline::testing::oracle_article();
}

std::string smoothed_article() {
    return replace_all(written_article(), "I'm Ada, and today", "I'm Ada; today");
}

json::Json article_artifact(std::string markdown) {
    return knowledge::to_json(knowledge::Article{.markdown = std::move(markdown)});
}

// The stages a page lists, in its order.
std::vector<std::string> stages_of(const Contents& contents) {
    std::vector<std::string> stages;
    stages.reserve(contents.findings.size());
    for (const StageFindings& stage : contents.findings) {
        stages.push_back(stage.stage);
    }
    return stages;
}

class ContentsTest : public ::testing::Test {
protected:
    [[nodiscard]] library::Job job(library::JobKind kind = library::JobKind::article) {
        return unwrap(jobs_.create("lecture.vtt", "max", 0, kind));
    }

    // `job`'s run of `stage`, done a second after the last run: `artifact`,
    // and what it could not do.
    void done(const library::Job& job, std::string_view stage, json::Json artifact,
              std::vector<std::string> findings = {}) {
        clock_.advance(1s);
        (void)unwrap(store_.commit(library::StageResult{.job_id = job.id,
                                                        .stage = std::string{stage},
                                                        .version = 1,
                                                        .key = next_key(job, stage),
                                                        .schema = std::format("artifacts/{}", stage),
                                                        .artifact = std::move(artifact),
                                                        .findings = std::move(findings),
                                                        .started_at = clock_.now()}));
    }

    // `job`'s run of `stage`, failed a second after the last run.
    void failed(const library::Job& job, std::string_view stage) {
        clock_.advance(1s);
        unwrap(store_.record_failure(job.id, stage, next_key(job, stage), "the pod is down", clock_.now()));
    }

    // Every stage of `job` done on the lecture, up to and with the write
    // stage's article.
    void done_through_writing(const library::Job& job) {
        done(job, pipeline::SourceStage::kName, pipeline::to_json(pipeline::testing::source()));
        done(job, pipeline::TranscriptStage::kName, knowledge::to_json(pipeline::testing::lecture()));
        done(job, pipeline::OutlineStage::kName, knowledge::to_json(pipeline::testing::lecture_outline()),
             {"section 2 was cut at a pause"});
        done(job, pipeline::AtomizeStage::kName, knowledge::to_json(pipeline::testing::lecture_ledger()));
        done(job, pipeline::PlanStage::kName, knowledge::to_json(pipeline::testing::lecture_plan()));
        done(job, pipeline::WriteStage::kName, article_artifact(written_article()));
    }

    [[nodiscard]] const library::StageStore& store() const { return store_; }

private:
    std::string next_key(const library::Job& job, std::string_view stage) {
        return std::format("{}/{}/{}", job.id, stage, ++runs_);
    }

    testkit::TempDir dir_;
    db::Database database_ = unwrap(library::open_database(dir_ / "library.db"));
    FakeClock clock_;
    library::Jobs jobs_{database_, clock_};
    library::StageStore store_{database_, clock_};
    std::size_t runs_ = 0;
};

TEST_F(ContentsTest, AJobDoneShowsEachStagesArtifactAndWhatItCouldNotDo) {
    const library::Job lecture = job();
    done_through_writing(lecture);
    done(lecture, pipeline::SeamsStage::kName, article_artifact(smoothed_article()));

    const Contents contents = unwrap(contents_of(lecture, store()));
    EXPECT_EQ(contents.job, lecture.id);
    EXPECT_EQ(contents.tier, "max");
    ASSERT_TRUE(contents.video.has_value());
    EXPECT_EQ(contents.video->title, "Weighing radium");
    EXPECT_EQ(contents.transcript, pipeline::testing::lecture());
    EXPECT_EQ(contents.outline, pipeline::testing::lecture_outline());
    EXPECT_EQ(contents.ledger, pipeline::testing::lecture_ledger());
    EXPECT_EQ(contents.plan, pipeline::testing::lecture_plan());
    EXPECT_THAT(stages_of(contents),
                ElementsAre("source", "transcript", "outline", "atomize", "plan", "write", "seams"));
    EXPECT_THAT(contents.findings.at(2).findings, ElementsAre("section 2 was cut at a pause"));
}

TEST_F(ContentsTest, TheArticleIsTheSeamsStagesWhereItRanElseTheWriteStages) {
    const library::Job lecture = job();
    done_through_writing(lecture);
    const Contents written = unwrap(contents_of(lecture, store()));
    ASSERT_TRUE(written.article.has_value());
    EXPECT_EQ(written.article->markdown, written_article()) << "the article so far";

    done(lecture, pipeline::SeamsStage::kName, article_artifact(smoothed_article()));
    const Contents smoothed = unwrap(contents_of(lecture, store()));
    ASSERT_TRUE(smoothed.article.has_value());
    EXPECT_EQ(smoothed.article->markdown, smoothed_article());
}

TEST_F(ContentsTest, AStageRunAgainIsShownAsItRanLast) {
    const library::Job lecture = job();
    done(lecture, pipeline::SourceStage::kName, pipeline::to_json(pipeline::testing::source()));
    done(lecture, pipeline::WriteStage::kName, article_artifact(written_article()),
         {"section 2: told as said"});
    done(lecture, pipeline::PlanStage::kName, knowledge::to_json(pipeline::testing::lecture_plan()));
    done(lecture, pipeline::WriteStage::kName, article_artifact(smoothed_article()));

    const Contents contents = unwrap(contents_of(lecture, store()));
    ASSERT_TRUE(contents.article.has_value());
    EXPECT_EQ(contents.article->markdown, smoothed_article()) << "the later run replaced the earlier";
    EXPECT_THAT(stages_of(contents), ElementsAre("source", "plan", "write")) << "in the order they ran";
    EXPECT_THAT(contents.findings.at(2).findings, IsEmpty()) << "the later run's findings";
}

TEST_F(ContentsTest, AFailedRunMadeNothingEvenWhereAnEarlierRunHadDoneIt) {
    const library::Job lecture = job();
    done(lecture, pipeline::SourceStage::kName, pipeline::to_json(pipeline::testing::source()));
    done(lecture, pipeline::TranscriptStage::kName, knowledge::to_json(pipeline::testing::lecture()));
    done(lecture, pipeline::OutlineStage::kName, knowledge::to_json(pipeline::testing::lecture_outline()));
    failed(lecture, pipeline::OutlineStage::kName);
    failed(lecture, pipeline::AtomizeStage::kName);

    const Contents contents = unwrap(contents_of(lecture, store()));
    EXPECT_TRUE(contents.transcript.has_value());
    EXPECT_FALSE(contents.outline.has_value()) << "run again, it failed";
    EXPECT_FALSE(contents.ledger.has_value());
    EXPECT_THAT(stages_of(contents), ElementsAre("source", "transcript"));
}

TEST_F(ContentsTest, AJobThatRanNothingIsItsJobAlone) {
    const library::Job lecture = job();
    const Contents contents = unwrap(contents_of(lecture, store()));
    EXPECT_EQ(contents, (Contents{.job = lecture.id, .tier = "max"}));
}

TEST_F(ContentsTest, AnEvalAnnotationHasNoArticleToShow) {
    const library::Job annotation = job(library::JobKind::annotation);
    const Result<Contents> contents = contents_of(annotation, store());
    EXPECT_ERRC(contents, Errc::invalid_argument);
    EXPECT_THAT(contents.error().message(), HasSubstr("rreport eval sheet"));
}

TEST_F(ContentsTest, AnArtifactThatDoesNotReadAsItsStagesIsAnError) {
    const library::Job lecture = job();
    done(lecture, pipeline::TranscriptStage::kName, json::Json{{"sentences", "none"}});
    EXPECT_FALSE(contents_of(lecture, store()).has_value());
}

}  // namespace
}  // namespace rr::report
