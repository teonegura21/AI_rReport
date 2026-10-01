// Replays a fuzz target's corpus without libFuzzer: LLVMFuzzerTestOneInput()
// on every file given, or found under a directory given, in a fixed order. A
// target aborts on a broken invariant, so the replay passes only when every
// input keeps them all.
//
// Linked with each fuzz target as `rr_<name>_fuzz_replay` and run by CTest
// (tests/fuzz/CMakeLists.txt): every push replays every corpus, under each
// compiler and sanitizer.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <vector>

// libFuzzer's entry point: its name is libFuzzer's.
extern "C" int LLVMFuzzerTestOneInput(  // NOLINT(readability-identifier-naming)
    const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::vector<fs::path> inputs;
    for (const char* argument : std::span{argv, static_cast<std::size_t>(argc)}.subspan(1)) {
        const fs::path path{argument};
        std::error_code error;
        if (fs::is_directory(path, error)) {
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator{path}) {
                if (entry.is_regular_file()) {
                    inputs.push_back(entry.path());
                }
            }
        } else {
            inputs.push_back(path);
        }
    }
    if (inputs.empty()) {
        (void)std::fputs("usage: <fuzz replay> <file or directory>...: no inputs found\n", stderr);
        return 1;
    }
    std::ranges::sort(inputs);

    for (const fs::path& input : inputs) {
        std::ifstream file{input, std::ios::binary};
        // Read through the stream's buffer, which leaves the stream's own eof
        // flag unset: success is an open file and no bad bit after.
        const std::string bytes = file.is_open() ? std::string{std::istreambuf_iterator<char>{file}, {}} : "";
        if (!file.is_open() || file.bad()) {
            (void)std::fprintf(stderr, "cannot read %s\n", input.c_str());
            return 1;
        }
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
    (void)std::printf("%zu inputs kept every invariant\n", inputs.size());
    return 0;
}
