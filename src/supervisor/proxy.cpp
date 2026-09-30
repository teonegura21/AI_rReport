#include <rr/supervisor/proxy.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <rr/core/strings.hpp>
#include <rr/log/log.hpp>
#include <rr/net/sse.hpp>

namespace rr::supervisor {

namespace {

using Clock = std::chrono::steady_clock;
using net::HttpServer;

bool is_success(int status) noexcept {
    return status >= 200 && status < 300;
}

bool is_event_stream(const net::Headers& headers) {
    const std::string_view type = headers.get("Content-Type").value_or("");
    constexpr std::string_view kEventStream = "text/event-stream";
    return type.size() >= kEventStream.size() &&
           iequals_ascii(type.substr(0, kEventStream.size()), kEventStream);
}

Error shutting_down() {
    return Error{Errc::unavailable, "the runtime is shutting down"};
}

// The engine's transfer failed before it answered.
Error unreachable(Error error) {
    return error.code() == Errc::network ? std::move(error).with_context("the model engine is unreachable")
                                         : std::move(error);
}

// The engine's transfer failed after part of its answer.
Error broke_off(Error error) {
    return std::move(error).with_context("the model engine's answer broke off");
}

// The comment that fills a silence. A function-local static: built once, on
// first use, from the one formatter that defines the wire format.
const std::string& keep_alive_comment() {
    static const std::string comment = net::format_sse_comment("keep-alive");
    return comment;
}

// The hand-over between the upstream thread, which runs the engine's transfer,
// and the downstream side: the handler, then the response's stream function.
// Every downstream wait takes a patience, so the waiter can re-check the
// client, the shutdown request and the keep-alive timer at least once a tick.
class Bridge {
public:
    // --- Upstream side --------------------------------------------------------
    Result<void> head(int status, const net::Headers& headers) {
        {
            const std::lock_guard lock(mutex_);
            status_ = status;
            headers_ = headers;
            head_ready_ = true;
        }
        changed_.notify_all();
        return ok();
    }

    // A piece of a 2xx body. Waits while the buffer is full; fails once the
    // downstream side has gone, which aborts the engine's transfer.
    Result<void> push(std::string_view chunk, const std::stop_token& stop) {
        std::unique_lock lock(mutex_);
        const bool room =
            changed_.wait(lock, stop, [&] { return buffered_ < kMaxBufferedBytes || abandoned_; });
        if (!room || abandoned_) {
            return fail(Errc::cancelled, "the client went away");
        }
        chunks_.emplace_back(chunk);
        buffered_ += chunk.size();
        lock.unlock();
        changed_.notify_all();
        return ok();
    }

    void finish(Result<net::HttpClient::Response> result) {
        {
            const std::lock_guard lock(mutex_);
            result_ = std::move(result);
            finished_ = true;
        }
        changed_.notify_all();
    }

    // --- Downstream side ------------------------------------------------------
    // True once the head arrived, or the transfer ended without one.
    bool wait_for_head(std::chrono::milliseconds patience) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [this] { return head_ready_ || finished_; });
    }

    bool wait_for_finish(std::chrono::milliseconds patience) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [this] { return finished_; });
    }

    struct Next {
        enum class Kind : std::uint8_t {
            chunk,
            end,
            silence
        };
        Kind kind = Kind::silence;
        std::string bytes{};
    };

    // The next piece of the body: `end` once the transfer is over (then see
    // result()), `silence` when nothing arrived within `patience`.
    Next next(std::chrono::milliseconds patience) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, patience, [this] { return !chunks_.empty() || finished_; })) {
            return Next{};
        }
        if (chunks_.empty()) {
            return Next{.kind = Next::Kind::end};
        }
        std::string chunk = std::move(chunks_.front());
        chunks_.pop_front();
        buffered_ -= chunk.size();
        lock.unlock();
        changed_.notify_all();  // there is room again
        return Next{.kind = Next::Kind::chunk, .bytes = std::move(chunk)};
    }

    // The downstream side has gone: stop accepting chunks.
    void abandon() {
        {
            const std::lock_guard lock(mutex_);
            abandoned_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool head_ready() const {
        const std::lock_guard lock(mutex_);
        return head_ready_;
    }
    [[nodiscard]] int status() const {
        const std::lock_guard lock(mutex_);
        return status_;
    }
    [[nodiscard]] net::Headers headers() const {
        const std::lock_guard lock(mutex_);
        return headers_;
    }
    // Final once a wait has seen the transfer finished; it never changes after.
    [[nodiscard]] const Result<net::HttpClient::Response>& result() const { return result_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<std::string> chunks_;
    std::size_t buffered_ = 0;
    int status_ = 0;
    net::Headers headers_;
    bool head_ready_ = false;
    bool finished_ = false;
    bool abandoned_ = false;
    Result<net::HttpClient::Response> result_ =
        fail(Errc::internal, "the engine's transfer has not finished");
};

// Everything one relayed call owns, shared by the handler and the response's
// stream function (std::function needs a copyable callable).
struct Exchange {
    Bridge bridge;
    Watchdog::InFlight in_flight;
    ProxyOptions options;
    // Declared last, so it is stopped and joined before the members above go
    // away. It holds a plain pointer to its Exchange, never a shared_ptr: the
    // last owner destroys this jthread, and a thread cannot join itself.
    std::jthread upstream;

    Exchange(Watchdog::InFlight guard, ProxyOptions chosen)
        : in_flight(std::move(guard))
        , options(std::move(chosen)) {}

    // Nobody wants the answer any more: stop the engine's work now.
    void cancel() {
        bridge.abandon();
        upstream.request_stop();
    }

    [[nodiscard]] bool shutting_down() const { return options.shutdown.stop_requested(); }

    // How long one wait may last: short enough to send keep-alives on time.
    [[nodiscard]] std::chrono::milliseconds tick() const {
        if (!options.keep_alive.has_value()) {
            return kMaxTick;
        }
        return std::clamp(*options.keep_alive / 2, std::chrono::milliseconds{1}, kMaxTick);
    }
};

// The downstream half, run by the response's stream function: writes what the
// engine produces, fills silences, and ends the body honestly.
class Downstream {
public:
    Downstream(Exchange& exchange, HttpServer::ChunkWriter& writer, bool event_stream)
        : exchange_(&exchange)
        , writer_(&writer)
        , event_stream_(event_stream) {}

    // The relay answered 200 before the engine did: wait for the engine's
    // answer, then relay it, or report its refusal in the stream.
    Result<void> after_early_answer() {
        if (!send(keep_alive_comment())) {  // bytes on the wire at once, and a first probe of the client
            return client_left();
        }
        Bridge& bridge = exchange_->bridge;
        while (!bridge.wait_for_head(exchange_->tick())) {
            if (auto over = keep_waiting(); over.has_value()) {
                return *over;
            }
        }
        if (bridge.head_ready() && is_success(bridge.status())) {
            if (!is_event_stream(bridge.headers())) {
                exchange_->cancel();
                return end_with(Error{
                    Errc::internal,
                    std::format("the model engine answered {} with '{}' where an event stream was promised",
                                bridge.status(),
                                bridge.headers().get("Content-Type").value_or("no content type"))});
            }
            return relay_body();
        }
        // An error status (or none at all) after our 200: the error travels
        // as the stream's only event, and the stream ends cleanly.
        while (!bridge.wait_for_finish(exchange_->tick())) {
            if (auto over = keep_waiting(); over.has_value()) {
                return *over;
            }
        }
        const auto& result = bridge.result();
        return end_with(result.has_value()
                            ? net::error_from_response(result->status, result->headers, result->body)
                            : unreachable(result.error()));
    }

    // Relays a 2xx body until it ends.
    Result<void> relay_body() {
        while (true) {
            const Bridge::Next next = exchange_->bridge.next(exchange_->tick());
            switch (next.kind) {
                case Bridge::Next::Kind::chunk:
                    if (!send(next.bytes)) {
                        return client_left();
                    }
                    relayed_ = true;
                    break;
                case Bridge::Next::Kind::silence:
                    if (auto over = keep_waiting(); over.has_value()) {
                        return *over;
                    }
                    break;
                case Bridge::Next::Kind::end: {
                    const auto& result = exchange_->bridge.result();
                    if (result.has_value()) {
                        return ok();
                    }
                    return end_with(broke_off(result.error()));
                }
            }
        }
    }

private:
    // One silent tick: keep the connection alive (or at least probe it).
    // std::nullopt means "keep waiting"; a value means the stream is over,
    // with that result: the runtime is stopping, or the client left. (Not a
    // plain Result: a stream that ends cleanly with an error event is ok(),
    // and must not read as "keep waiting".)
    std::optional<Result<void>> keep_waiting() {
        if (writer_->stopping() || exchange_->shutting_down()) {
            exchange_->cancel();
            return end_with(shutting_down());
        }
        const auto keep_alive = exchange_->options.keep_alive;
        const bool due = keep_alive.has_value() && Clock::now() - last_write_ >= *keep_alive;
        if (due && event_stream_ && lines_.at_line_start()) {
            if (!send(keep_alive_comment())) {
                return client_left();
            }
        } else if (!writer_->write({})) {  // sends nothing: a probe
            return client_left();
        }
        return std::nullopt;
    }

    // Ends the body with `error`. An event stream reports it as an event when
    // one fits here. If part of the engine's answer went out, the transfer is
    // then cut short, so that answer cannot pass for complete; if not, the
    // error event is the whole body, complete in itself.
    Result<void> end_with(Error error) {
        log::warn("model call to {} ended with an error: {}", exchange_->options.url, error.describe());
        if (event_stream_ && lines_.between_events()) {
            if (!send(net::format_sse_event(net::error_body(error)))) {
                return client_left();
            }
            if (!relayed_) {
                return ok();
            }
        }
        return fail(std::move(error));
    }

    Result<void> client_left() {
        exchange_->cancel();
        log::debug("model call to {}: the client went away", exchange_->options.url);
        return fail(Errc::cancelled, "the client went away");
    }

    bool send(std::string_view bytes) {
        if (!writer_->write(bytes)) {
            return false;
        }
        lines_.feed(bytes);
        last_write_ = Clock::now();
        return true;
    }

    Exchange* exchange_;
    HttpServer::ChunkWriter* writer_;
    bool event_stream_;
    bool relayed_ = false;  // part of the engine's body went out
    net::SseLineTracker lines_;
    Clock::time_point last_write_ = Clock::now();  // the headers just went out
};

// The engine answered 2xx: relay its body.
HttpServer::Response relayed_answer(const std::shared_ptr<Exchange>& exchange) {
    const net::Headers headers = exchange->bridge.headers();
    const bool event_stream = is_event_stream(headers);
    HttpServer::Response response{
        .status = exchange->bridge.status(),
        .content_type = std::string{headers.get("Content-Type").value_or("application/octet-stream")},
        .stream = [exchange, event_stream](HttpServer::ChunkWriter& writer) -> Result<void> {
            return Downstream{*exchange, writer, event_stream}.relay_body();
        }};
    if (event_stream) {
        response.headers.add("Cache-Control", "no-cache");
        response.headers.add("X-Accel-Buffering", "no");  // ask reverse proxies not to buffer the stream
    }
    return response;
}

// A streamed call whose engine is slow to answer: answer 200 on its behalf.
HttpServer::Response early_answer(const std::shared_ptr<Exchange>& exchange) {
    return HttpServer::Response::event_stream([exchange](HttpServer::ChunkWriter& writer) -> Result<void> {
        return Downstream{*exchange, writer, /*event_stream=*/true}.after_early_answer();
    });
}

// Why a handler waiting for the engine should stop waiting, if it should.
std::optional<Error> reason_to_give_up(const Exchange& exchange, const HttpServer::Request& request) {
    if (exchange.shutting_down()) {
        return shutting_down();
    }
    if (request.client_gone && request.client_gone()) {
        return Error{Errc::cancelled, "the client went away"};  // nobody will read the answer
    }
    return std::nullopt;
}

// The engine refused, or could not be reached: pass that on with its status.
HttpServer::Response failure_answer(Exchange& exchange, const HttpServer::Request& request) {
    // An error body follows its head at once, but still wait a tick at a time.
    while (!exchange.bridge.wait_for_finish(exchange.tick())) {
        if (auto reason = reason_to_give_up(exchange, request); reason.has_value()) {
            exchange.cancel();
            return HttpServer::Response::error(*reason);
        }
    }
    const auto& result = exchange.bridge.result();
    if (!result.has_value()) {
        return HttpServer::Response::error(unreachable(result.error()));
    }
    HttpServer::Response response{
        .status = result->status,
        .body = result->body,
        .content_type = std::string{result->headers.get("Content-Type").value_or("application/json")}};
    if (const auto retry = result->headers.get("Retry-After"); retry.has_value()) {
        response.headers.add("Retry-After", std::string{*retry});
    }
    return response;
}

}  // namespace

HttpServer::Response proxy(const net::HttpClient& client, ProxyOptions options,
                           const HttpServer::Request& request, Watchdog::InFlight in_flight) {
    net::HttpClient::Request upstream_request{.method = request.method,
                                              .url = options.url,
                                              .body = request.body,
                                              .idle_timeout = options.idle_timeout};
    // Only these two headers are forwarded (see the header comment).
    if (const auto type = request.headers.get("Content-Type"); type.has_value()) {
        upstream_request.headers.add("Content-Type", std::string{*type});
    }
    upstream_request.headers.add("Accept", std::string{request.headers.get("Accept").value_or("*/*")});

    auto exchange = std::make_shared<Exchange>(std::move(in_flight), std::move(options));
    Exchange* raw = exchange.get();
    exchange->upstream = std::jthread{
        [raw, &client, upstream_request = std::move(upstream_request)](const std::stop_token& stop) {
            auto result = client.stream(
                upstream_request,
                {.on_head = [raw](int status,
                                  const net::Headers& headers) { return raw->bridge.head(status, headers); },
                 .on_chunk = [raw, &stop](std::string_view chunk) { return raw->bridge.push(chunk, stop); }},
                stop);
            raw->bridge.finish(std::move(result));
        }};

    // Wait for the engine's answer, a tick at a time.
    const bool streamed = exchange->options.keep_alive.has_value();
    const auto answer_by = Clock::now() + exchange->options.head_patience;
    while (!exchange->bridge.wait_for_head(exchange->tick())) {
        if (auto reason = reason_to_give_up(*exchange, request); reason.has_value()) {
            exchange->cancel();
            log::debug("model call to {} abandoned before the engine answered: {}", exchange->options.url,
                       reason->describe());
            return HttpServer::Response::error(*reason);
        }
        if (streamed && Clock::now() >= answer_by) {
            return early_answer(exchange);
        }
    }
    if (!exchange->bridge.head_ready() || !is_success(exchange->bridge.status())) {
        return failure_answer(*exchange, request);
    }
    return relayed_answer(exchange);
}

}  // namespace rr::supervisor
