#include <rr/proc/line_splitter.hpp>

#include <algorithm>
#include <utility>

#include <rr/core/assert.hpp>

namespace rr::proc {

LineSplitter::LineSplitter(LineCallback on_line, std::size_t max_line)
    : on_line_(std::move(on_line))
    , max_line_(max_line) {
    RR_ASSERT(max_line_ > 0, "a line limit of zero would never make progress");
}

void LineSplitter::feed(std::string_view bytes) {
    while (!bytes.empty()) {
        const std::size_t end = bytes.find_first_of("\r\n");
        append(bytes.substr(0, end));
        if (end == std::string_view::npos) {
            return;  // the rest of the line arrives in a later chunk
        }
        emit();
        bytes.remove_prefix(end + 1);
    }
}

void LineSplitter::finish() {
    emit();
}

void LineSplitter::append(std::string_view text) {
    while (!text.empty()) {
        const std::size_t room = max_line_ - partial_.size();
        const std::size_t taken = std::min(room, text.size());
        partial_.append(text.substr(0, taken));
        text.remove_prefix(taken);
        if (partial_.size() == max_line_) {
            emit();  // an overlong line is delivered in max_line-sized pieces
        }
    }
}

void LineSplitter::emit() {
    if (partial_.empty()) {
        return;
    }
    if (on_line_) {
        on_line_(partial_);
    }
    partial_.clear();
}

}  // namespace rr::proc
