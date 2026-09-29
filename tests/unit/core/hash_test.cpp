#include <rr/core/hash.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace rr {
namespace {

// Published test vectors.
constexpr std::string_view kBlake3Empty = "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262";
constexpr std::string_view kBlake3Abc = "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";
constexpr std::string_view kSha256Empty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::string_view kSha256Abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

class TempFile {
public:
    explicit TempFile(const std::string& content)
        : path_(std::filesystem::temp_directory_path() /
                ("rr_hash_test_" + std::to_string(std::random_device{}()) + ".bin")) {
        std::ofstream(path_, std::ios::binary) << content;
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    ~TempFile() { std::filesystem::remove(path_); }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

TEST(Blake3, MatchesPublishedVectors) {
    EXPECT_EQ(blake3("").hex(), kBlake3Empty);
    EXPECT_EQ(blake3("abc").hex(), kBlake3Abc);
}

TEST(Sha256, MatchesPublishedVectors) {
    EXPECT_EQ(sha256("").hex(), kSha256Empty);
    EXPECT_EQ(sha256("abc").hex(), kSha256Abc);
}

TEST(Hashers, IncrementalEqualsOneShot_AndFinalizeDoesNotConsume) {
    Blake3Hasher b3;
    b3.update("a");
    b3.update("bc");
    EXPECT_EQ(b3.finalize().hex(), kBlake3Abc);
    EXPECT_EQ(b3.finalize().hex(), kBlake3Abc);  // still valid after finalize()

    Sha256Hasher sha;
    sha.update("ab");
    sha.update("c");
    EXPECT_EQ(sha.finalize().hex(), kSha256Abc);
    sha.update("d");  // continue after finalize()
    EXPECT_EQ(sha.finalize(), sha256("abcd"));
}

TEST(Digest, HexRoundTripAndValidation) {
    const auto parsed = Blake3Digest::from_hex(kBlake3Abc);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, blake3("abc"));
    EXPECT_EQ(Blake3Digest::from_hex(std::string(kBlake3Abc).substr(2)).error().code(),
              Errc::invalid_argument);
    std::string bad(kBlake3Abc);
    bad[10] = 'z';
    EXPECT_EQ(Blake3Digest::from_hex(bad).error().code(), Errc::invalid_argument);
    // Uppercase hex is accepted and normalized to lowercase.
    std::string upper(kSha256Abc);
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    EXPECT_EQ(Sha256Digest::from_hex(upper).value().hex(), kSha256Abc);
}

TEST(FileHashing, EqualsHashingTheSameBytesInMemory) {
    std::string content((3 * 1024 * 1024) + 17, '\0');  // spans several 1 MiB chunks
    std::mt19937 random{42};
    for (char& c : content) {
        c = static_cast<char>(random() & 0xFFU);
    }
    const TempFile file{content};
    EXPECT_EQ(blake3_file(file.path()).value(), blake3(content));
    EXPECT_EQ(sha256_file(file.path()).value(), sha256(content));
}

TEST(FileHashing, MissingFile_IsAnIoError) {
    EXPECT_EQ(blake3_file("/definitely/not/here.bin").error().code(), Errc::io);
}

TEST(FileHashing, RequestedStop_Cancels) {
    const TempFile file{"data"};
    // Non-const: request_stop() is a non-const member in the C++ standard.
    std::stop_source source;  // NOLINT(misc-const-correctness)
    source.request_stop();
    EXPECT_EQ(sha256_file(file.path(), source.get_token()).error().code(), Errc::cancelled);
}

TEST(Hex, FromHexRejectsOddLengthAndBadCharacters) {
    EXPECT_EQ(from_hex("abc").error().code(), Errc::invalid_argument);
    EXPECT_EQ(from_hex("zz").error().code(), Errc::invalid_argument);
    EXPECT_EQ(from_hex("00ff").value(), std::string("\x00\xff", 2));
}

}  // namespace
}  // namespace rr
