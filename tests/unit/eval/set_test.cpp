// The eval set: read strictly, every mistake named where it is.
#include <rr/eval/set.hpp>

#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::eval {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

constexpr std::string_view kSet = R"toml(# Two videos.
[[video]]
id = "fed-talk-1"
title = "Monetary policy, explained"
url = "https://www.youtube.com/watch?v=abc123XYZ_0"
type = "talk"
captions = "manual"
split = "dev"
licence = "us-public-domain"
attribution = "Board of Governors of the Federal Reserve System"

[[video]]
id = "mars-chat"
title = "A conversation on Mars"
url = "https://www.youtube.com/watch?v=def456"
type = "conversation"
captions = "automatic"
split = "test"
licence = "cc-by"
attribution = "NASA"
snapshot = "4f2a9c"
)toml";

TEST(EvalSet, IsReadVideoByVideoInItsFilesOrder) {
    const EvalSet set = unwrap(parse_set("m2", kSet));
    EXPECT_EQ(set.name, "m2");
    ASSERT_EQ(set.videos.size(), 2U);
    EXPECT_EQ(set.videos[0], (EvalVideo{.id = "fed-talk-1",
                                        .title = "Monetary policy, explained",
                                        .url = "https://www.youtube.com/watch?v=abc123XYZ_0",
                                        .type = VideoType::talk,
                                        .captions = media::CaptionKind::manual,
                                        .split = Split::dev,
                                        .licence = Licence::us_public_domain,
                                        .attribution = "Board of Governors of the Federal Reserve System"}));
    EXPECT_EQ(set.videos[1], (EvalVideo{.id = "mars-chat",
                                        .title = "A conversation on Mars",
                                        .url = "https://www.youtube.com/watch?v=def456",
                                        .type = VideoType::conversation,
                                        .captions = media::CaptionKind::automatic,
                                        .split = Split::test,
                                        .licence = Licence::cc_by,
                                        .attribution = "NASA",
                                        .snapshot = "4f2a9c"}));
}

TEST(EvalSet, FindsAVideoByItsId) {
    const EvalSet set = unwrap(parse_set("m2", kSet));
    ASSERT_NE(set.find("mars-chat"), nullptr);
    EXPECT_EQ(set.find("mars-chat")->title, "A conversation on Mars");
    EXPECT_EQ(set.find("mars"), nullptr);
}

TEST(EvalSet, SpellsItsChoicesAsItsFilesDo) {
    EXPECT_EQ(to_string(VideoType::talk), "talk");
    EXPECT_EQ(to_string(VideoType::conversation), "conversation");
    EXPECT_EQ(to_string(VideoType::tutorial), "tutorial");
    EXPECT_EQ(to_string(VideoType::narrative), "narrative");
    EXPECT_EQ(to_string(Split::dev), "dev");
    EXPECT_EQ(to_string(Split::test), "test");
    EXPECT_EQ(to_string(Licence::cc_by), "cc-by");
    EXPECT_EQ(to_string(Licence::us_public_domain), "us-public-domain");
}

TEST(EvalSet, AVideoIdCanNameADirectoryAndNothingElse) {
    for (const std::string_view id : {"a", "7", "fed-talk-1", "x-"}) {
        EXPECT_TRUE(valid_video_id(id)) << id;
    }
    EXPECT_TRUE(valid_video_id(std::string(64, 'a')));
    EXPECT_FALSE(valid_video_id(std::string(65, 'a'))) << "64 at most";
    for (const std::string_view id : {"", "-a", "Fed", "a_b", "a.b", "..", "a/b", "a b", "caf\u00E9"}) {
        EXPECT_FALSE(valid_video_id(id)) << id;
    }
}

// A set with one mistake, and what the message must say of it.
struct Mistake {
    std::string_view why;
    std::string_view find;     // in kSet, once
    std::string_view replace;  // what it becomes
    std::string_view message;
};

TEST(EvalSet, EveryMistakeIsRefusedNamingItAndWhereItIs) {
    const std::vector<Mistake> mistakes{
        Mistake{"an unknown key", "attribution = \"NASA\"", "attribution = \"NASA\"\nlisence = \"cc-by\"",
                "eval/sets/m2.toml, video 2: unknown key 'lisence'"},
        Mistake{"a key missing", "split = \"test\"\n", "",
                "eval/sets/m2.toml, video 2: 'split' must be a text, and it is missing or empty"},
        Mistake{"an empty text", "title = \"A conversation on Mars\"", "title = \"\"",
                "video 2: 'title' must be a text, and it is missing or empty"},
        Mistake{"a number for a text", "title = \"A conversation on Mars\"", "title = 3",
                "video 2: 'title' must be a text in quotes"},
        Mistake{"a type out of its choices", "type = \"conversation\"", "type = \"podcast\"",
                "'type' must be one of: talk, conversation, tutorial, narrative"},
        Mistake{"captions out of their choices", "captions = \"automatic\"", "captions = \"auto\"",
                "'captions' must be one of: manual, automatic"},
        Mistake{"a split out of its choices", "split = \"test\"", "split = \"train\"",
                "'split' must be one of: dev, test"},
        Mistake{"a licence the eval may not use", "licence = \"cc-by\"", "licence = \"cc-by-nc\"",
                "'licence' must be one of: cc-by, us-public-domain"},
        Mistake{"an id that cannot name a directory", "id = \"mars-chat\"", "id = \"Mars/Chat\"",
                "video 2: id 'Mars/Chat' must be 1 to 64 of a-z, 0-9 and '-'"},
        Mistake{"two videos with one id", "id = \"mars-chat\"", "id = \"fed-talk-1\"",
                "video 2: a second video with the id 'fed-talk-1'"},
        Mistake{"a key outside the videos", "# Two videos.", "name = \"m2\"",
                "eval/sets/m2.toml: unknown key 'name'"},
        Mistake{"videos that are not tables", kSet, "video = \"fed-talk-1\"\n",
                "'video' must be [[video]] tables"},
        Mistake{"videos that are a list of values", kSet, "video = [1, 2]\n",
                "'video' must be [[video]] tables"},
        Mistake{"broken TOML", "title = \"A conversation on Mars\"", "title = \"A conversation",
                "eval/sets/m2.toml, line 14"}};
    for (const Mistake& mistake : mistakes) {
        std::string text{kSet};
        const std::size_t at = text.find(mistake.find);
        ASSERT_NE(at, std::string::npos) << mistake.why;
        ASSERT_EQ(text.find(mistake.find, at + 1), std::string::npos)
            << mistake.why << ": the edit must be unique";
        text.replace(at, mistake.find.size(), mistake.replace);

        const Result<EvalSet> set = parse_set("m2", text);
        EXPECT_ERRC(set, Errc::invalid_argument) << mistake.why;
        if (!set.has_value()) {
            EXPECT_THAT(set.error().message(), HasSubstr(mistake.message)) << mistake.why;
        }
    }
}

TEST(EvalSet, IsReadFromItsFileAmongTheSets) {
    const testkit::TempDir root;
    (void)root.write_file("sets/m2.toml", kSet);
    EXPECT_EQ(unwrap(read_set(root.path(), "m2")).videos.size(), 2U);
    EXPECT_ERRC(read_set(root.path(), "m3"), Errc::not_found);
}

}  // namespace
}  // namespace rr::eval
