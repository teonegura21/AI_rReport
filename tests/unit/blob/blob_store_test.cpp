#include <rr/blob/blob_store.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::blob {
namespace {

using namespace std::chrono_literals;
using testkit::unwrap;
namespace fs = std::filesystem;

std::size_t count_files(const fs::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        count += entry.is_regular_file() ? 1U : 0U;
    }
    return count;
}

// Files in the store proper, excluding writes in progress under .tmp/.
std::size_t count_blobs(const BlobStore& store) {
    return count_files(store.root()) - count_files(store.root() / ".tmp");
}

std::string random_bytes(std::size_t size, unsigned seed) {
    std::mt19937 engine{seed};
    std::uniform_int_distribution<int> byte{0, 255};
    std::string bytes(size, '\0');
    for (char& c : bytes) {
        c = static_cast<char>(byte(engine));
    }
    return bytes;
}

mode_t permissions_of(const fs::path& path) {
    struct stat info{};
    EXPECT_EQ(::stat(path.c_str(), &info), 0) << path;
    return info.st_mode & 0777U;
}

// ---------------------------------------------------------------------------
// Writing and reading
// ---------------------------------------------------------------------------
TEST(BlobStore, StoresContentUnderItsHash) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir / "blobs"));

    const BlobRef ref = unwrap(store.put_text("a sentence worth keeping"));
    EXPECT_EQ(ref.hash, blake3("a sentence worth keeping"));
    EXPECT_EQ(ref.size, 24U);

    const std::string hex = ref.hash.hex();
    EXPECT_EQ(store.path_of(ref.hash), store.root() / hex.substr(0, 2) / hex);
    EXPECT_TRUE(store.contains(ref.hash));
    EXPECT_EQ(unwrap(store.stat(ref.hash)), ref);
    EXPECT_EQ(unwrap(store.read_text(ref.hash)), "a sentence worth keeping");
    EXPECT_EQ(count_files(store.root() / ".tmp"), 0U) << "no temp file may be left behind";
}

TEST(BlobStore, EmptyContentIsAValidBlob) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const BlobRef ref = unwrap(store.put_text(""));
    EXPECT_EQ(ref.hash, blake3(""));
    EXPECT_EQ(ref.size, 0U);
    EXPECT_EQ(unwrap(store.read_text(ref.hash)), "");
}

TEST(BlobStore, IdenticalContentIsStoredOnce) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const BlobRef first = unwrap(store.put_text("same audio"));
    const BlobRef second = unwrap(store.put_text("same audio"));
    EXPECT_EQ(first, second);
    EXPECT_EQ(count_blobs(store), 1U);
    BlobWriter writer = unwrap(store.begin_write());
    ASSERT_OK(writer.write("same audio"));
    ASSERT_OK(writer.commit());
    EXPECT_EQ(count_files(store.root() / ".tmp"), 0U) << "discarded by commit(), not by the destructor";
    EXPECT_EQ(count_files(store.root() / ".tmp"), 0U) << "the redundant copy is discarded";
}

TEST(BlobStore, StreamedWritesHashLikeOneShotWrites) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    BlobWriter writer = unwrap(store.begin_write());
    ASSERT_OK(writer.write("data "));
    ASSERT_OK(writer.write("curation"));
    EXPECT_EQ(writer.size(), 13U);
    const BlobRef ref = unwrap(writer.commit());
    EXPECT_EQ(ref.hash, blake3("data curation"));
    EXPECT_EQ(ref.size, 13U);
}

TEST(BlobStore, CopiesFilesLargerThanOneChunk) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir / "blobs"));
    const std::string content = random_bytes((std::size_t{3} << 20U) + 17, 7);  // 3 MiB + a partial chunk
    const auto source = dir.write_file("audio.opus", content);

    const BlobRef ref = unwrap(store.put_file(source));
    EXPECT_EQ(ref.hash, blake3(content));
    EXPECT_EQ(ref.size, content.size());
    EXPECT_EQ(unwrap(store.read_text(ref.hash)), content);
    EXPECT_OK(store.verify(ref.hash));
}

TEST(BlobStore, MissingSourceFileIsNotFound) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    EXPECT_ERRC(store.put_file(dir / "nope.wav"), Errc::not_found);
    EXPECT_ERRC(store.stat(blake3("never stored")), Errc::not_found);
    EXPECT_ERRC(store.read_text(blake3("never stored")), Errc::not_found);
    EXPECT_FALSE(store.contains(blake3("never stored")));
}

TEST(BlobStore, ReadsRespectTheirSizeLimit) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const BlobRef ref = unwrap(store.put_text("0123456789"));
    EXPECT_ERRC(store.read_bytes(ref.hash, 9), Errc::invalid_argument);
    EXPECT_EQ(unwrap(store.read_bytes(ref.hash, 10)).size(), 10U);
}

TEST(BlobStore, FilesAndDirectoriesArePrivate) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir / "blobs"));
    const BlobRef ref = unwrap(store.put_text("the user's media"));
    EXPECT_EQ(permissions_of(store.path_of(ref.hash)), 0600U);
    EXPECT_EQ(permissions_of(store.path_of(ref.hash).parent_path()), 0700U);
    EXPECT_EQ(permissions_of(store.root() / ".tmp"), 0700U);
}

// ---------------------------------------------------------------------------
// Nothing partial or unverified is ever published
// ---------------------------------------------------------------------------
TEST(BlobWriter, AMismatchedExpectedHashPublishesNothing) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    BlobWriter writer = unwrap(store.begin_write());
    ASSERT_OK(writer.write("bytes damaged in transit"));
    EXPECT_ERRC(writer.commit(blake3("the bytes the sender meant")), Errc::integrity);
    EXPECT_FALSE(store.contains(blake3("bytes damaged in transit")));
    EXPECT_EQ(count_files(store.root()), 0U);
}

TEST(BlobWriter, AMatchingExpectedHashPublishes) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    BlobWriter writer = unwrap(store.begin_write());
    ASSERT_OK(writer.write("an upload"));
    EXPECT_EQ(unwrap(writer.commit(blake3("an upload"))).hash, blake3("an upload"));
}

TEST(BlobWriter, AnAbandonedWriterLeavesNoTrace) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    {
        BlobWriter writer = unwrap(store.begin_write());
        ASSERT_OK(writer.write("half an upload"));
        EXPECT_EQ(count_files(store.root() / ".tmp"), 1U);
    }  // destroyed without commit(), e.g. the client disconnected
    EXPECT_EQ(count_files(store.root()), 0U);
}

TEST(BlobWriter, TheSizeLimitIsEnforcedAndSpendsTheWriter) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    BlobWriter writer = unwrap(store.begin_write(/*max_size=*/4));
    ASSERT_OK(writer.write("abc"));
    EXPECT_ERRC(writer.write("de"), Errc::invalid_argument);
    EXPECT_EQ(count_files(store.root()), 0U) << "the rejected bytes are deleted at once";
    EXPECT_ERRC(writer.write("f"), Errc::internal) << "a failed writer accepts nothing more";
    EXPECT_ERRC(writer.commit(), Errc::internal);
}

TEST(BlobWriter, CommitTwiceIsRefused) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    BlobWriter writer = unwrap(store.begin_write());
    ASSERT_OK(writer.write("once"));
    ASSERT_OK(writer.commit());
    EXPECT_ERRC(writer.commit(), Errc::internal);
    EXPECT_TRUE(store.contains(blake3("once"))) << "the published blob survives the refused call";
}

TEST(BlobStore, ACancelledCopyPublishesNothing) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir / "blobs"));
    const auto source = dir.write_file("video.webm", random_bytes(1000, 3));
    // Non-const: request_stop() is a non-const member in the C++ standard.
    std::stop_source cancel;  // NOLINT(misc-const-correctness)
    cancel.request_stop();
    EXPECT_ERRC(store.put_file(source, cancel.get_token()), Errc::cancelled);
    EXPECT_EQ(count_files(store.root()), 0U);
}

TEST(BlobStore, ConcurrentWritersOfTheSameContentAgree) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const std::string content = random_bytes(std::size_t{1} << 20U, 11);
    constexpr int kWriters = 8;
    std::vector<Result<BlobRef>> results(kWriters, fail(Errc::internal, "not run"));
    {
        std::vector<std::jthread> threads;
        threads.reserve(kWriters);
        for (int index = 0; index < kWriters; ++index) {
            threads.emplace_back(
                [&, index] { results[static_cast<std::size_t>(index)] = store.put_text(content); });
        }
    }
    for (const auto& result : results) {
        ASSERT_OK(result);
        EXPECT_EQ(result->hash, blake3(content));
    }
    EXPECT_EQ(count_blobs(store), 1U);
    EXPECT_EQ(count_files(store.root() / ".tmp"), 0U);
    EXPECT_OK(store.verify(blake3(content)));
}

// ---------------------------------------------------------------------------
// Integrity and housekeeping
// ---------------------------------------------------------------------------
TEST(BlobStore, VerifyDetectsCorruption) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const BlobRef ref = unwrap(store.put_text("pristine"));
    EXPECT_OK(store.verify(ref.hash));
    std::ofstream(store.path_of(ref.hash), std::ios::binary | std::ios::trunc) << "tampered";
    EXPECT_ERRC(store.verify(ref.hash), Errc::integrity);
}

TEST(BlobStore, StoringContentAgainHealsATruncatedBlob) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const BlobRef ref = unwrap(store.put_text("complete content"));
    fs::resize_file(store.path_of(ref.hash), 3);  // e.g. damaged by a disk problem
    EXPECT_EQ(unwrap(store.put_text("complete content")), ref);
    EXPECT_OK(store.verify(ref.hash));
}

TEST(BlobStore, RemoveReportsWhetherABlobWasThere) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const BlobRef ref = unwrap(store.put_text("short-lived"));
    EXPECT_TRUE(unwrap(store.remove(ref.hash)));
    EXPECT_FALSE(unwrap(store.remove(ref.hash)));
    EXPECT_FALSE(store.contains(ref.hash));
}

TEST(BlobStore, RemoveUnusedSinceKeepsTouchedBlobsAndForeignFiles) {
    const testkit::TempDir dir{"rr_blob_test"};
    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    const BlobRef idle = unwrap(store.put_text("idle upload"));
    const BlobRef used = unwrap(store.put_text("frame still in use"));
    const auto two_hours_ago = fs::file_time_type::clock::now() - 2h;
    fs::last_write_time(store.path_of(idle.hash), two_hours_ago);
    fs::last_write_time(store.path_of(used.hash), two_hours_ago);
    ASSERT_OK(store.touch(used.hash));  // e.g. read by a job just now
    const auto foreign = dir.write_file(store.path_of(idle.hash).parent_path() / "notes.txt", "not a blob");
    fs::last_write_time(foreign, two_hours_ago);

    EXPECT_EQ(unwrap(store.remove_unused_since(fs::file_time_type::clock::now() - 1h)), 1U);
    EXPECT_FALSE(store.contains(idle.hash));
    EXPECT_TRUE(store.contains(used.hash));
    EXPECT_TRUE(fs::exists(foreign)) << "files that are not blobs are never deleted";
    EXPECT_ERRC(store.touch(idle.hash), Errc::not_found);
}

TEST(BlobStore, OpenSweepsOnlyStaleTempFiles) {
    const testkit::TempDir dir{"rr_blob_test"};
    const auto stale = dir.write_file(".tmp/01OLDCRASHEDWRITE", "left by a crash");
    const auto active = dir.write_file(".tmp/01ACTIVEWRITE", "another process, mid-write");
    fs::last_write_time(stale, fs::file_time_type::clock::now() - BlobStore::kStaleTempAge - 1min);

    const BlobStore store = unwrap(BlobStore::open(dir.path()));
    EXPECT_FALSE(fs::exists(stale));
    EXPECT_TRUE(fs::exists(active));
}

}  // namespace
}  // namespace rr::blob
