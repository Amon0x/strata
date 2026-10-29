#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "data/json.hpp"

namespace strata::desktop {

/** One `strata.platform` request: the session that sent it, its number, kind and payload. */
struct PlatformRequest final {
    std::string client;
    std::uint64_t number = 0U;
    std::string kind;
    data::JsonValue payload;
    /** Set once the session that sent it has gone: nothing is answered or said to it after. */
    std::shared_ptr<const std::atomic<bool>> closed;
};

/**
 * Where platform services leave what they have to say for the next frame: answers to requests,
 * and events said unasked on a channel. Any thread may call; what is meant for a session that has
 * gone is dropped.
 */
class PlatformOutbox {
  public:
    virtual ~PlatformOutbox() = default;
    /** Answers `request` with `fields`, at the next pump. */
    virtual void reply(const PlatformRequest& request, data::JsonValue::Object fields) = 0;
    /** Says `data` on `channel` to the session that sent `request`, at the next pump. */
    virtual void say(const PlatformRequest& request, const std::string& channel,
                     data::JsonValue data) = 0;
};

/**
 * A family of platform requests (`http.`, `file.`, ...) that the frame loop starts and that finish
 * later, mostly on threads of their own.
 */
class PlatformService {
  public:
    virtual ~PlatformService() = default;
    /** Whether requests of this kind are this service's. */
    [[nodiscard]] virtual bool serves(std::string_view kind) const noexcept = 0;
    /** Starts `request` on the frame loop's thread, without blocking it. */
    virtual void run(const PlatformRequest& request) = 0;
    /** Runs on the frame loop's thread after each pump. */
    virtual void tick() {}
    /** Stops everything `client` started, its session having gone. */
    virtual void close_client(std::string_view client) = 0;
};

/** A resource id's file, for platform requests that name one instead of a path. */
using ResourcePathResolver = std::function<std::wstring(std::string_view)>;

[[nodiscard]] std::unique_ptr<PlatformService> make_network_service(PlatformOutbox& outbox);
[[nodiscard]] std::unique_ptr<PlatformService> make_file_service(PlatformOutbox& outbox,
                                                                 ResourcePathResolver resolve);
[[nodiscard]] std::unique_ptr<PlatformService> make_audio_service(PlatformOutbox& outbox,
                                                                  ResourcePathResolver resolve);

/**
 * Threads that run blocking work (a download, a file read) in the order it arrives, started as
 * work needs them up to a limit. Each joins in COM's single-threaded apartment, which the shell's
 * file operations ask for. Destruction drops work not yet started and joins the threads once
 * their current task returns, so the owner cancels long tasks first.
 */
class PlatformWorkers final {
  public:
    explicit PlatformWorkers(std::size_t limit);
    ~PlatformWorkers();

    PlatformWorkers(const PlatformWorkers&) = delete;
    PlatformWorkers& operator=(const PlatformWorkers&) = delete;

    /** Runs `task` on a worker; a task that throws is the task's own business. */
    void submit(std::function<void()> task);

  private:
    void work(std::stop_token stop);

    std::size_t limit_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::function<void()>> tasks_;
    std::size_t idle_ = 0U;
    std::vector<std::jthread> threads_;
};

/** The pieces of platform requests that need no system, kept apart so tests can reach them. */
namespace platform_io {

[[nodiscard]] std::string utf8(std::wstring_view value);
[[nodiscard]] std::wstring wide(std::string_view value);

[[nodiscard]] std::string text_field(const data::JsonValue& payload, std::string_view name,
                                     std::string fallback = {});
[[nodiscard]] bool flag_field(const data::JsonValue& payload, std::string_view name, bool fallback);
[[nodiscard]] std::optional<double> number_field(const data::JsonValue& payload,
                                                 std::string_view name);

/**
 * The path a request names in `path` (or `field`), or else the one its event brings: a string, a
 * dialog's `path`, the first of a drop's `files`. Empty when there is none.
 */
[[nodiscard]] std::string requested_path(const data::JsonValue& payload,
                                         std::string_view field = "path");

/**
 * A path as the file system takes it: `%VARIABLE%`s expanded, absolute paths (drive or UNC) and
 * URLs kept, anything else resolved as a resource id by `resolve` (kept as it is without one).
 */
[[nodiscard]] std::filesystem::path resolve_path(std::string_view path,
                                                 const ResourcePathResolver& resolve);

/** Whether `name` matches `pattern` (`*` any run, `?` one character), ignoring case. */
[[nodiscard]] bool glob_match(std::wstring_view pattern, std::wstring_view name);

[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> bytes);
/** The bytes `text` encodes (whitespace skipped, padding optional); nothing when it is not base64.
 */
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text);

/** `value` as compact JSON on one line, as a message or request body carries it. */
[[nodiscard]] std::string compact_json(const data::JsonValue& value);

/** Milliseconds since 1970-01-01 UTC for a Windows file time (100 ns since 1601). */
[[nodiscard]] double unix_millis(std::uint64_t file_time) noexcept;

/** An HTTP request's body as bytes, and the content type it implies (empty: none). */
struct HttpBody final {
    std::string bytes;
    std::string content_type;
};

/**
 * The body a request gives: text as it is (UTF-8 text), any other value as JSON. No body (or
 * null) sends nothing.
 */
[[nodiscard]] HttpBody http_body(const data::JsonValue* body);

/**
 * The header block to send: the request's `headers` object (string, number and boolean values),
 * then the body's content type unless the headers name one. Lines end in CRLF.
 */
[[nodiscard]] std::string http_header_block(const data::JsonValue* headers, const HttpBody& body);

/**
 * A response's headers as an object keyed by lower-case name, from the raw block (status line
 * first); repeated headers join with ", ".
 */
[[nodiscard]] data::JsonValue::Object http_response_headers(std::string_view raw);

/**
 * The answer to a request that got a response: `ok` (a 2xx status), `status`, `statusText`,
 * `headers`, and the body as `json` (asked for with `as: "json"`, or by a JSON content type) or
 * as `body` text. A body that should be JSON and is not answers `ok: false` with an `error`
 * and the text.
 */
[[nodiscard]] data::JsonValue::Object http_answer(std::int64_t status, std::string status_text,
                                                  data::JsonValue::Object headers, std::string body,
                                                  std::string_view as);

/** The answer to a request that got no response: `ok: false`, status 0, and the `error`. */
[[nodiscard]] data::JsonValue::Object http_failure(std::string error);

/** A WebSocket text message as a channel event: `{ type: "message", text, json? }`. */
[[nodiscard]] data::JsonValue socket_message(std::string text);

} // namespace platform_io

} // namespace strata::desktop
