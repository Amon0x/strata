#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>
#include <bcrypt.h>
#include <mmeapi.h>
#include <objbase.h>

#include "platform_io.hpp"

namespace {

using strata::data::JsonValue;
using strata::desktop::PlatformOutbox;
using strata::desktop::PlatformRequest;
using namespace strata::desktop::platform_io;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

[[nodiscard]] const JsonValue& field(const JsonValue& value, const std::string_view name) {
    const JsonValue* found = value.find(name);
    if (found == nullptr)
        throw std::runtime_error("missing field " + std::string(name) + " in " +
                                 strata::data::encode_canonical_json(value));
    return *found;
}

[[nodiscard]] std::string text(const JsonValue& value, const std::string_view name) {
    const JsonValue& found = field(value, name);
    check(found.string() != nullptr, "field is not text: " + std::string(name));
    return *found.string();
}

[[nodiscard]] bool flag(const JsonValue& value, const std::string_view name) {
    const JsonValue& found = field(value, name);
    check(found.boolean() != nullptr, "field is not a boolean: " + std::string(name));
    return *found.boolean();
}

/** What services said, in order, for a test to wait on. */
class RecordingOutbox final : public PlatformOutbox {
  public:
    struct Said final {
        std::uint64_t request = 0U;
        std::string channel;
        JsonValue data;
    };

    void reply(const PlatformRequest& request, JsonValue::Object fields) override {
        record(Said{request.number, {}, JsonValue(std::move(fields))});
    }
    void say(const PlatformRequest&, const std::string& channel, JsonValue data) override {
        record(Said{0U, channel, std::move(data)});
    }

    /** The answer to request `number`, waiting for it. */
    [[nodiscard]] JsonValue
    answer(const std::uint64_t number,
           const std::chrono::milliseconds within = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        std::optional<JsonValue> found;
        const bool arrived = changed_.wait_for(lock, within, [&] {
            for (const Said& said : said_) {
                if (said.request == number && said.channel.empty()) {
                    found = said.data;
                    return true;
                }
            }
            return false;
        });
        if (!arrived)
            throw std::runtime_error("no answer to request " + std::to_string(number));
        return *found;
    }
    /** Whether request `number` has an answer, without waiting. */
    [[nodiscard]] bool answered(const std::uint64_t number) {
        const std::scoped_lock lock(mutex_);
        for (const Said& said : said_) {
            if (said.request == number && said.channel.empty())
                return true;
        }
        return false;
    }
    /** The first event on `channel` that `wanted` accepts, waiting for it. */
    template <typename Wanted>
    [[nodiscard]] JsonValue
    event(const std::string& channel, Wanted wanted,
          const std::chrono::milliseconds within = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        std::optional<JsonValue> found;
        const bool arrived = changed_.wait_for(lock, within, [&] {
            for (const Said& said : said_) {
                if (said.channel == channel && wanted(said.data)) {
                    found = said.data;
                    return true;
                }
            }
            return false;
        });
        if (!arrived)
            throw std::runtime_error("no such event on " + channel);
        return *found;
    }
    void clear() {
        const std::scoped_lock lock(mutex_);
        said_.clear();
    }

  private:
    void record(Said said) {
        {
            const std::scoped_lock lock(mutex_);
            said_.push_back(std::move(said));
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Said> said_;
};

[[nodiscard]] PlatformRequest request(const std::uint64_t number, std::string kind,
                                      JsonValue::Object fields, std::string client = "test") {
    fields.emplace_back("kind", JsonValue(kind));
    return PlatformRequest{std::move(client), number, std::move(kind), JsonValue(std::move(fields)),
                           std::make_shared<std::atomic<bool>>(false)};
}

// ---- the parts that need no system ----

void test_glob() {
    check(glob_match(L"*.png", L"photo.PNG"), "a star should match a run, ignoring case");
    check(!glob_match(L"*.png", L"photo.png.txt"), "a pattern matches the whole name");
    check(glob_match(L"a?c*", L"abcdef"), "a question mark matches one character");
    check(glob_match(L"*", L""), "a star matches nothing too");
    check(glob_match(L"*a*b", L"xxaxxab"), "a star backtracks");
    check(!glob_match(L"a*b", L"acbx"), "a pattern must end where the name does");
}

void test_base64() {
    const std::string sample = "Strata!\x01\xFF";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(sample.data());
    const std::string encoded = base64_encode({bytes, sample.size()});
    check(encoded == "U3RyYXRhIQH/", "base64 encoding");
    const std::optional<std::vector<std::uint8_t>> decoded = base64_decode(encoded);
    check(decoded.has_value() && std::string(decoded->begin(), decoded->end()) == sample,
          "base64 round trip");
    check(base64_decode("U3Ry\nYXRh").has_value(), "base64 skips line breaks");
    check(!base64_decode("U3R*").has_value(), "base64 rejects other characters");
    check(base64_encode({bytes, 1U}) == "Uw==", "base64 pads a lone byte");
}

void test_http_parts() {
    check(http_body(nullptr).bytes.empty(), "no body sends nothing");
    const JsonValue plain("hello");
    check(http_body(&plain).content_type.starts_with("text/plain"), "text is sent as text");
    const JsonValue object(
        JsonValue::Object{{"b", JsonValue(std::int64_t{2})}, {"a", JsonValue(true)}});
    const HttpBody json = http_body(&object);
    check(json.bytes == R"({"a":true,"b":2})", "an object is sent as JSON");
    check(json.content_type.starts_with("application/json"), "JSON says so");

    const JsonValue headers(JsonValue::Object{{"Accept", JsonValue("application/json")},
                                              {"X-Count", JsonValue(std::int64_t{3})},
                                              {"Bad\nName", JsonValue("x")},
                                              {"X-Injected", JsonValue("a\r\nEvil: yes")}});
    const std::string block = http_header_block(&headers, json);
    check(block == "Accept: application/json\r\nX-Count: 3\r\nContent-Type: "
                   "application/json; charset=utf-8\r\n",
          "the header block: " + block);
    const JsonValue typed(JsonValue::Object{{"content-type", JsonValue("application/x-thing")}});
    check(http_header_block(&typed, json) == "content-type: application/x-thing\r\n",
          "a content type the request names wins");

    const JsonValue::Object parsed = http_response_headers(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nSet-Cookie: a=1\r\n"
        "set-cookie: b=2\r\nX-Empty:\r\n\r\n");
    const JsonValue parsed_value(parsed);
    check(text(parsed_value, "content-type") == "application/json", "header names are lower case");
    check(text(parsed_value, "set-cookie") == "a=1, b=2", "repeated headers join");
    check(text(parsed_value, "x-empty").empty(), "an empty header is kept");

    const JsonValue answer(http_answer(200, "OK", parsed, R"([1,2])", ""));
    check(flag(answer, "ok"), "200 is ok");
    check(field(answer, "json").array() != nullptr, "a JSON content type answers json");
    check(answer.find("body") == nullptr, "json answers have no body text");
    const JsonValue as_text(http_answer(404, "Not Found", {}, "missing", ""));
    check(!flag(as_text, "ok") && text(as_text, "body") == "missing", "404 is not ok");
    const JsonValue forced(http_answer(200, "OK", {}, "{\"a\":1}", "json"));
    check(field(field(forced, "json"), "a").integer() != nullptr, "as json parses any type");
    const JsonValue broken(http_answer(200, "OK", parsed, "not json", ""));
    check(!flag(broken, "ok") && text(broken, "body") == "not json" &&
              text(broken, "error").starts_with("the response is not JSON"),
          "a body that is not JSON says so");
    const JsonValue failed(http_failure("could not connect"));
    check(!flag(failed, "ok") && *field(failed, "status").integer() == 0, "a failure");

    const JsonValue message = socket_message(R"({"from":"server"})");
    check(text(message, "type") == "message" && field(message, "json").object() != nullptr,
          "a JSON message brings json");
    check(socket_message("plain words").find("json") == nullptr, "plain text brings none");
}

void test_paths() {
    const JsonValue named(JsonValue::Object{{"path", JsonValue("a.txt")}});
    check(requested_path(named) == "a.txt", "a named path");
    const JsonValue dialog(JsonValue::Object{
        {"event", JsonValue(JsonValue::Object{{"path", JsonValue("C:\\picked.txt")}})}});
    check(requested_path(dialog) == "C:\\picked.txt", "a dialog's path");
    const JsonValue dropped(JsonValue::Object{
        {"event", JsonValue(JsonValue::Object{
                      {"files", JsonValue(JsonValue::Array{JsonValue("C:\\one"),
                                                           JsonValue("C:\\two")})}})}});
    check(requested_path(dropped) == "C:\\one", "a drop's first file");
    check(requested_path(JsonValue(JsonValue::Object{})).empty(), "no path");

    const strata::desktop::ResourcePathResolver resolve = [](const std::string_view id) {
        return L"C:\\resources\\" + wide(id);
    };
    check(resolve_path("assets/a.wav", resolve) ==
              std::filesystem::path(L"C:\\resources\\assets\\a.wav"),
          "a resource id resolves under the root");
    check(resolve_path("D:/music/song.mp3", resolve) ==
              std::filesystem::path(L"D:\\music\\song.mp3"),
          "an absolute path stays");
    check(resolve_path("http://localhost/a.mp3", resolve).native() == L"http://localhost/a.mp3",
          "a URL stays as written");
    wchar_t temp[MAX_PATH]{};
    GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH);
    check(resolve_path("%TEMP%\\x.txt", resolve) == std::filesystem::path(temp) / L"x.txt",
          "environment variables expand");
    check(unix_millis(116'444'736'000'000'000ULL + 10'000ULL * 1500ULL) == 1500.0,
          "file times count from 1970 in milliseconds");
}

// ---- files ----

void test_files(const std::filesystem::path& root) {
    RecordingOutbox outbox;
    std::unique_ptr<strata::desktop::PlatformService> files =
        strata::desktop::make_file_service(outbox, {});
    const std::string folder = utf8((root / L"nested" / L"deeper").native());
    const std::string note = folder + "\\note.txt";

    files->run(request(1U, "file.write",
                       {{"path", JsonValue(note)},
                        {"text", JsonValue("first line\n")},
                        {"createDirectories", JsonValue(true)}}));
    const JsonValue written = outbox.answer(1U);
    check(flag(written, "ok") && *field(written, "size").integer() == 11, "file.write wrote");

    files->run(request(2U, "file.write",
                       {{"path", JsonValue(note)},
                        {"text", JsonValue("second line\n")},
                        {"append", JsonValue(true)}}));
    check(flag(outbox.answer(2U), "ok"), "file.write appended");

    // The path may come from the event, as a dialog's answer brings it.
    files->run(request(3U, "file.read",
                       {{"event", JsonValue(JsonValue::Object{{"path", JsonValue(note)}})}}));
    const JsonValue read = outbox.answer(3U);
    check(text(read, "text") == "first line\nsecond line\n", "file.read read both lines");
    check(*field(read, "size").integer() == 23 && *field(read, "modified").number() > 1.6e12,
          "file.read says the size and time");

    const std::string data = folder + "\\data.json";
    files->run(request(
        4U, "file.write",
        {{"path", JsonValue(data)},
         {"json", JsonValue(JsonValue::Array{JsonValue(std::int64_t{1}), JsonValue("two")})}}));
    check(flag(outbox.answer(4U), "ok"), "file.write wrote JSON");
    files->run(request(5U, "file.read", {{"path", JsonValue(data)}, {"as", JsonValue("json")}}));
    const JsonValue parsed = outbox.answer(5U);
    check(field(parsed, "json").array() != nullptr && field(parsed, "json").array()->size() == 2U,
          "file.read parsed JSON");
    files->run(request(6U, "file.read", {{"path", JsonValue(data)}, {"as", JsonValue("base64")}}));
    // Files keep JSON readable: canonical, one value to a line.
    const std::optional<std::vector<std::uint8_t>> raw =
        base64_decode(text(outbox.answer(6U), "base64"));
    check(raw.has_value() && std::string(raw->begin(), raw->end()) ==
                                 strata::data::encode_canonical_json(JsonValue(JsonValue::Array{
                                     JsonValue(std::int64_t{1}), JsonValue("two")})),
          "file.read as base64 brings the bytes written");
    files->run(
        request(7U, "file.write",
                {{"path", JsonValue(folder + "\\bytes.bin")}, {"base64", JsonValue("AAEC/w==")}}));
    check(*field(outbox.answer(7U), "size").integer() == 4, "file.write decoded base64");

    files->run(request(8U, "file.list",
                       {{"path", JsonValue(utf8(root.native()))},
                        {"pattern", JsonValue("*.json")},
                        {"recursive", JsonValue(true)}}));
    const JsonValue listed = outbox.answer(8U);
    const JsonValue::Array& entries = *field(listed, "entries").array();
    check(entries.size() == 1U && text(entries.front(), "name") == "data.json" &&
              !flag(entries.front(), "directory"),
          "file.list found the JSON file below");
    files->run(request(9U, "file.list", {{"path", JsonValue(folder)}}));
    check(field(outbox.answer(9U), "entries").array()->size() == 3U, "file.list lists everything");

    files->run(request(10U, "file.info", {{"path", JsonValue(note)}}));
    const JsonValue info = outbox.answer(10U);
    check(flag(info, "exists") && !flag(info, "directory") && *field(info, "size").integer() == 23,
          "file.info of a file");
    files->run(request(11U, "file.info", {{"path", JsonValue(folder + "\\absent")}}));
    check(!flag(outbox.answer(11U), "exists"), "file.info of nothing");
    files->run(request(12U, "file.read", {{"path", JsonValue(folder + "\\absent")}}));
    const JsonValue missing = outbox.answer(12U);
    check(!flag(missing, "ok") && text(missing, "error").starts_with("could not open"),
          "file.read of nothing fails");

    // Watching: a new file, a change, a rename and a removal arrive on the channel.
    files->run(request(13U, "file.watch",
                       {{"path", JsonValue(folder)}, {"channel", JsonValue("watched")}}));
    check(flag(outbox.answer(13U), "ok"), "file.watch started");
    const std::string fresh = folder + "\\fresh.txt";
    files->run(request(14U, "file.write", {{"path", JsonValue(fresh)}, {"text", JsonValue("x")}}));
    static_cast<void>(outbox.answer(14U));
    const auto about = [&fresh](const std::string_view type) {
        return [type, &fresh](const JsonValue& data) {
            return text(data, "type") == type && text(data, "path") == fresh;
        };
    };
    static_cast<void>(outbox.event("watched", about("added")));
    std::filesystem::rename(wide(fresh), wide(folder + "\\renamed.txt"));
    const JsonValue renamed = outbox.event(
        "watched", [](const JsonValue& data) { return text(data, "type") == "renamed"; });
    check(text(renamed, "path").ends_with("renamed.txt") &&
              text(renamed, "oldPath").ends_with("fresh.txt"),
          "a rename says both names");
    std::filesystem::remove(wide(folder + "\\renamed.txt"));
    static_cast<void>(outbox.event("watched", [](const JsonValue& data) {
        return text(data, "type") == "removed" && text(data, "path").ends_with("renamed.txt");
    }));

    // A watched file hears about itself only.
    files->run(
        request(15U, "file.watch", {{"path", JsonValue(note)}, {"channel", JsonValue("one")}}));
    check(flag(outbox.answer(15U), "ok"), "file.watch of a file started");
    files->run(request(16U, "file.write", {{"path", JsonValue(data)}, {"text", JsonValue("[]")}}));
    static_cast<void>(outbox.answer(16U));
    files->run(request(
        17U, "file.write",
        {{"path", JsonValue(note)}, {"text", JsonValue("more\n")}, {"append", JsonValue(true)}}));
    static_cast<void>(outbox.answer(17U));
    const JsonValue changed =
        outbox.event("one", [](const JsonValue& data) { return text(data, "type") == "changed"; });
    check(text(changed, "path") == note, "the watched file changed");
    files->run(request(18U, "file.unwatch", {{"channel", JsonValue("one")}}));
    check(flag(outbox.answer(18U), "ok"), "file.unwatch stopped it");
    files->run(request(19U, "file.unwatch", {{"channel", JsonValue("one")}}));
    check(!flag(outbox.answer(19U), "ok"), "file.unwatch of nothing");
    // The rest stop with the session, and the service ends cleanly.
    files->close_client("test");
    files.reset();
}

// ---- HTTP and WebSockets against a loopback server ----

[[nodiscard]] std::string sha1(const std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    std::array<std::uint8_t, 20> digest{};
    check(
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0U)),
        "sha1");
    BCryptHash(algorithm, nullptr, 0U, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
               static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0U);
    return base64_encode(digest);
}

[[nodiscard]] bool receive_exact(const SOCKET connection, char* const into,
                                 const std::size_t count) {
    std::size_t done = 0U;
    while (done < count) {
        const int got = recv(connection, into + done, static_cast<int>(count - done), 0);
        if (got <= 0)
            return false;
        done += static_cast<std::size_t>(got);
    }
    return true;
}

/** A frame from the client (always masked): its opcode and payload; nothing once it hangs up. */
[[nodiscard]] std::optional<std::pair<int, std::string>> read_frame(const SOCKET connection) {
    unsigned char head[2];
    if (!receive_exact(connection, reinterpret_cast<char*>(head), 2U))
        return std::nullopt;
    std::uint64_t length = head[1] & 0x7FU;
    if (length == 126U) {
        unsigned char size[2];
        if (!receive_exact(connection, reinterpret_cast<char*>(size), 2U))
            return std::nullopt;
        length = (static_cast<std::uint64_t>(size[0]) << 8U) | size[1];
    } else if (length == 127U) {
        return std::nullopt;
    }
    unsigned char mask[4]{};
    if ((head[1] & 0x80U) != 0U && !receive_exact(connection, reinterpret_cast<char*>(mask), 4U))
        return std::nullopt;
    std::string payload(static_cast<std::size_t>(length), '\0');
    if (!receive_exact(connection, payload.data(), payload.size()))
        return std::nullopt;
    for (std::size_t index = 0U; index < payload.size(); ++index)
        payload[index] = static_cast<char>(payload[index] ^ mask[index % 4U]);
    return std::make_pair(head[0] & 0x0F, std::move(payload));
}

void write_frame(const SOCKET connection, const int opcode, const std::string_view payload) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126U) {
        frame += static_cast<char>(payload.size());
    } else {
        frame += static_cast<char>(126);
        frame += static_cast<char>((payload.size() >> 8U) & 0xFFU);
        frame += static_cast<char>(payload.size() & 0xFFU);
    }
    frame += payload;
    send(connection, frame.data(), static_cast<int>(frame.size()), 0);
}

/** A close frame's payload: the code, then the reason. */
[[nodiscard]] std::string close_payload(const int code, const std::string_view reason) {
    std::string payload;
    payload += static_cast<char>((code >> 8) & 0xFF);
    payload += static_cast<char>(code & 0xFF);
    payload += reason;
    return payload;
}

/** A tiny HTTP/1.1 server on 127.0.0.1 that answers by path. */
class LoopbackServer final {
  public:
    LoopbackServer() {
        WSADATA data{};
        check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "winsock");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
              "bind");
        int size = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        check(listen(listener_, 8) == 0, "listen");
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackServer() {
        stopping_ = true;
        closesocket(listener_);
        thread_.join();
        for (std::thread& connection : connections_)
            connection.join();
        WSACleanup();
    }
    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] std::string url(const std::string_view path,
                                  const std::string_view scheme = "http") const {
        return std::string(scheme) + "://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

  private:
    void serve() {
        for (;;) {
            const SOCKET connection = accept(listener_, nullptr, nullptr);
            if (connection == INVALID_SOCKET)
                return;
            connections_.emplace_back([this, connection] { handle(connection); });
        }
    }

    void handle(const SOCKET connection) {
        std::string request;
        char buffer[4096];
        std::size_t body_at = std::string::npos;
        std::size_t length = 0U;
        while (body_at == std::string::npos || request.size() < body_at + length) {
            const int got = recv(connection, buffer, sizeof(buffer), 0);
            if (got <= 0)
                break;
            request.append(buffer, static_cast<std::size_t>(got));
            if (body_at == std::string::npos) {
                const std::size_t end = request.find("\r\n\r\n");
                if (end != std::string::npos) {
                    body_at = end + 4U;
                    std::string head = request.substr(0U, end);
                    for (char& character : head)
                        character =
                            static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                    if (const std::size_t at = head.find("content-length:");
                        at != std::string::npos)
                        length = std::stoul(head.substr(at + 15U));
                }
            }
        }
        const std::string line = request.substr(0U, request.find("\r\n"));
        std::string type = "text/plain";
        std::string body;
        int status = 200;
        if (line.starts_with("GET /people ")) {
            type = "application/json";
            body = R"([{"name":"Ada","role":"engineer"},{"name":"Grace","role":"admiral"}])";
        } else if (line.starts_with("POST /echo ")) {
            std::string head = request.substr(0U, body_at);
            const std::size_t at = head.find("Content-Type: ");
            type = at == std::string::npos
                       ? "text/plain"
                       : head.substr(at + 14U, head.find("\r\n", at) - at - 14U);
            body = request.substr(body_at);
        } else if (line.starts_with("GET /chat ")) {
            chat(connection, request.substr(0U, body_at));
            return;
        } else if (line.starts_with("GET /slow ")) {
            // Never answers until the client hangs up or the server stops.
            while (!stopping_) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(connection, &readable);
                timeval wait{0, 50'000};
                if (select(0, &readable, nullptr, nullptr, &wait) > 0 &&
                    recv(connection, buffer, sizeof(buffer), 0) <= 0) {
                    break;
                }
            }
            closesocket(connection);
            return;
        } else {
            status = 404;
            body = "missing";
        }
        const std::string response =
            "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") +
            "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nX-Served-By: loopback\r\nConnection: close\r\n\r\n" + body;
        send(connection, response.data(), static_cast<int>(response.size()), 0);
        shutdown(connection, SD_SEND);
        closesocket(connection);
    }

    /**
     * A WebSocket: greets with JSON, echoes text, and on "bye" closes itself with 1000 "bye now";
     * a close from the client is echoed.
     */
    static void chat(const SOCKET connection, const std::string& head) {
        const std::size_t at = head.find("Sec-WebSocket-Key: ");
        const std::string key = head.substr(at + 19U, head.find("\r\n", at) - at - 19U);
        std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                               sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") + "\r\n";
        if (head.find("Sec-WebSocket-Protocol: chat.v1") != std::string::npos)
            response += "Sec-WebSocket-Protocol: chat.v1\r\n";
        response += "\r\n";
        send(connection, response.data(), static_cast<int>(response.size()), 0);
        write_frame(connection, 0x1, R"({"hello":1})");
        for (;;) {
            const std::optional<std::pair<int, std::string>> frame = read_frame(connection);
            if (!frame.has_value())
                break;
            const auto& [opcode, payload] = *frame;
            if (opcode == 0x8) {
                write_frame(connection, 0x8, payload.substr(0U, 2U));
                break;
            }
            if (opcode == 0x9) {
                write_frame(connection, 0xA, payload);
            } else if (opcode == 0x1 && payload == "bye") {
                write_frame(connection, 0x8, close_payload(1000, "bye now"));
            } else if (opcode == 0x1) {
                write_frame(connection, 0x1, payload);
            }
        }
        shutdown(connection, SD_BOTH);
        closesocket(connection);
    }

    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t port_ = 0U;
    std::atomic<bool> stopping_ = false;
    std::thread thread_;
    std::vector<std::thread> connections_;
};

void test_http() {
    LoopbackServer server;
    RecordingOutbox outbox;
    std::unique_ptr<strata::desktop::PlatformService> network =
        strata::desktop::make_network_service(outbox);
    check(network->serves("http.request") && network->serves("socket.open") &&
              !network->serves("file.read"),
          "the network service serves http and sockets");

    network->run(request(1U, "http.request", {{"url", JsonValue(server.url("/people"))}}));
    const JsonValue people = outbox.answer(1U);
    check(flag(people, "ok") && *field(people, "status").integer() == 200, "GET answered 200");
    check(text(people, "statusText") == "OK", "the status text");
    check(text(field(people, "headers"), "x-served-by") == "loopback", "the response headers");
    const JsonValue::Array& list = *field(people, "json").array();
    check(list.size() == 2U && text(list[1], "name") == "Grace", "the JSON body parsed");

    network->run(request(2U, "http.request",
                         {{"url", JsonValue(server.url("/echo"))},
                          {"method", JsonValue("post")},
                          {"body", JsonValue(JsonValue::Object{{"hello", JsonValue("world")}})}}));
    const JsonValue echoed = outbox.answer(2U);
    check(flag(echoed, "ok"), "POST answered");
    check(text(field(echoed, "json"), "hello") == "world",
          "POST sent JSON and the echo came back as JSON");

    network->run(request(3U, "http.request",
                         {{"url", JsonValue(server.url("/echo"))},
                          {"method", JsonValue("POST")},
                          {"body", JsonValue("plain words")},
                          {"as", JsonValue("text")}}));
    check(text(outbox.answer(3U), "body") == "plain words", "POST text, read as text");

    network->run(request(4U, "http.request", {{"url", JsonValue(server.url("/nothing"))}}));
    const JsonValue missing = outbox.answer(4U);
    check(!flag(missing, "ok") && *field(missing, "status").integer() == 404 &&
              text(missing, "body") == "missing",
          "404 answers not ok, with its body");

    network->run(request(5U, "http.request",
                         {{"url", JsonValue(server.url("/slow"))}, {"timeout", JsonValue(300.0)}}));
    const JsonValue slow = outbox.answer(5U);
    check(!flag(slow, "ok") && text(slow, "error") == "the request timed out",
          "a request past its timeout fails: " + strata::data::encode_canonical_json(slow));

    network->run(request(6U, "http.request", {{"url", JsonValue("ftp://example.com/")}}));
    check(text(outbox.answer(6U), "error").starts_with("an HTTP URL"), "only http and https");

    // A session that goes while its request waits hears nothing, and nothing crashes.
    network->run(request(7U, "http.request", {{"url", JsonValue(server.url("/slow"))}}, "gone"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto started = std::chrono::steady_clock::now();
    network->close_client("gone");
    network->run(request(8U, "http.request", {{"url", JsonValue(server.url("/people"))}}));
    check(flag(outbox.answer(8U), "ok"), "requests go on after another session left");
    check(!outbox.answered(7U), "the cancelled request was not answered");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "cancelling did not wait for the request");

    // Sockets: a server that is not there fails to open, on the socket's channel too.
    network->run(request(9U, "socket.open",
                         {{"id", JsonValue("chat")}, {"url", JsonValue("ws://127.0.0.1:9/")}}));
    const JsonValue refused = outbox.answer(9U);
    check(!flag(refused, "ok") && text(refused, "id") == "chat", "socket.open of nothing fails");
    static_cast<void>(
        outbox.event("chat", [](const JsonValue& data) { return text(data, "type") == "error"; }));
    network->run(
        request(10U, "socket.send", {{"id", JsonValue("nothing")}, {"text", JsonValue("hi")}}));
    check(!flag(outbox.answer(10U), "ok"), "socket.send without a socket fails");
    network->tick();
    // The destructor cancels what is left.
    network->run(request(11U, "http.request", {{"url", JsonValue(server.url("/slow"))}}));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    network.reset();
}

void test_sockets() {
    LoopbackServer server;
    RecordingOutbox outbox;
    std::unique_ptr<strata::desktop::PlatformService> network =
        strata::desktop::make_network_service(outbox);
    const auto typed = [](const std::string_view type) {
        return [type](const JsonValue& data) { return text(data, "type") == type; };
    };

    network->run(request(1U, "socket.open",
                         {{"id", JsonValue("ws")},
                          {"url", JsonValue(server.url("/chat", "ws"))},
                          {"protocols", JsonValue(JsonValue::Array{JsonValue("chat.v1")})}}));
    const JsonValue opened = outbox.answer(1U);
    check(flag(opened, "ok") && text(opened, "protocol") == "chat.v1",
          "socket.open connected with the protocol: " +
              strata::data::encode_canonical_json(opened));
    static_cast<void>(outbox.event("ws", typed("open")));
    const JsonValue greeting = outbox.event("ws", typed("message"));
    check(*field(field(greeting, "json"), "hello").integer() == 1, "the greeting parsed as JSON");

    network->run(
        request(2U, "socket.send", {{"id", JsonValue("ws")}, {"text", JsonValue("ping")}}));
    check(flag(outbox.answer(2U), "ok"), "socket.send sent text");
    static_cast<void>(outbox.event("ws", [](const JsonValue& data) {
        return text(data, "type") == "message" && text(data, "text") == "ping";
    }));
    network->run(
        request(3U, "socket.send",
                {{"id", JsonValue("ws")},
                 {"json", JsonValue(JsonValue::Object{{"a", JsonValue(std::int64_t{7})}})}}));
    check(flag(outbox.answer(3U), "ok"), "socket.send sent JSON");
    static_cast<void>(outbox.event("ws", [](const JsonValue& data) {
        return text(data, "type") == "message" && text(data, "text") == R"({"a":7})";
    }));

    // The server closes: the code and reason arrive, and the socket is gone.
    network->run(request(4U, "socket.send", {{"id", JsonValue("ws")}, {"text", JsonValue("bye")}}));
    const JsonValue closed = outbox.event("ws", typed("closed"));
    check(*field(closed, "code").integer() == 1000 && text(closed, "reason") == "bye now",
          "the server's close: " + strata::data::encode_canonical_json(closed));
    for (int attempt = 0; attempt < 100; ++attempt) {
        network->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    network->run(
        request(5U, "socket.send", {{"id", JsonValue("ws")}, {"text", JsonValue("late")}}));
    check(!flag(outbox.answer(5U), "ok"), "a closed socket sends nothing");

    // This side closes.
    network->run(
        request(6U, "socket.open",
                {{"id", JsonValue("two")}, {"url", JsonValue(server.url("/chat", "ws"))}}));
    check(flag(outbox.answer(6U), "ok"), "a second socket opened");
    network->run(request(7U, "socket.close",
                         {{"id", JsonValue("two")},
                          {"code", JsonValue(std::int64_t{1000})},
                          {"reason", JsonValue("done")}}));
    check(flag(outbox.answer(7U), "ok"), "socket.close sent the close");
    check(*field(outbox.event("two", typed("closed")), "code").integer() == 1000,
          "the close completed");

    // A session that goes takes its socket, which says nothing more.
    network->run(request(
        8U, "socket.open",
        {{"id", JsonValue("three")}, {"url", JsonValue(server.url("/chat", "ws"))}}, "gone"));
    check(flag(outbox.answer(8U), "ok"), "a third socket opened");
    static_cast<void>(outbox.event("three", typed("message")));
    network->close_client("gone");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    bool said_closed = true;
    try {
        static_cast<void>(outbox.event("three", typed("closed"), std::chrono::milliseconds(100)));
    } catch (const std::exception&) {
        said_closed = false;
    }
    check(!said_closed, "an abandoned socket says nothing");
    // One left open ends with the service.
    network->run(
        request(9U, "socket.open",
                {{"id", JsonValue("four")}, {"url", JsonValue(server.url("/chat", "ws"))}}));
    check(flag(outbox.answer(9U), "ok"), "a fourth socket opened");
    network.reset();
}

// ---- sound, muted ----

void test_audio(const std::filesystem::path& resources) {
    RecordingOutbox outbox;
    std::unique_ptr<strata::desktop::PlatformService> audio = strata::desktop::make_audio_service(
        outbox, [resources](const std::string_view id) { return (resources / wide(id)).native(); });
    const auto settle = [&](const std::uint64_t number) {
        for (int attempt = 0; attempt < 1000 && !outbox.answered(number); ++attempt) {
            audio->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return outbox.answer(number, std::chrono::milliseconds(1));
    };
    // A click many times over itself: each press is a sound of its own.
    for (std::uint64_t press = 1U; press <= 8U; ++press) {
        audio->run(request(
            press, "audio.play",
            {{"source", JsonValue("assets/strata/media/click.wav")}, {"muted", JsonValue(true)}}));
    }
    for (std::uint64_t press = 1U; press <= 8U; ++press) {
        const JsonValue played = settle(press);
        check(flag(played, "ok") && *field(played, "duration").number() > 0.0,
              "each click played: " + strata::data::encode_canonical_json(played));
    }
    // Once they end, their engines play the click again at once.
    for (int attempt = 0; attempt < 200; ++attempt) {
        audio->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    audio->run(request(
        9U, "audio.play",
        {{"source", JsonValue("assets/strata/media/click.wav")}, {"muted", JsonValue(true)}}));
    check(outbox.answered(9U), "a click played again answers at once");

    audio->run(request(10U, "audio.play",
                       {{"source", JsonValue("assets/strata/media/loop.m4a")},
                        {"id", JsonValue("music")},
                        {"loop", JsonValue(true)},
                        {"volume", JsonValue(0.0)},
                        {"muted", JsonValue(true)}}));
    const JsonValue music = settle(10U);
    check(flag(music, "ok") && *field(music, "duration").number() > 7.0 &&
              text(music, "id") == "music",
          "the music loop opened: " + strata::data::encode_canonical_json(music));
    audio->run(request(11U, "audio.pause", {{"id", JsonValue("music")}}));
    check(flag(settle(11U), "paused"), "audio.pause paused it");
    audio->run(request(12U, "audio.set", {{"id", JsonValue("music")}, {"rate", JsonValue(1.5)}}));
    check(flag(settle(12U), "ok"), "audio.set changed its speed");
    audio->run(request(13U, "audio.resume", {{"id", JsonValue("music")}}));
    check(!flag(settle(13U), "paused"), "audio.resume played it on");
    audio->run(request(14U, "audio.stop", {{"id", JsonValue("music")}}));
    check(flag(settle(14U), "stopped"), "audio.stop stopped it");
    audio->run(request(15U, "audio.stop", {{"id", JsonValue("music")}}));
    check(!flag(settle(15U), "stopped"), "audio.stop of nothing stops nothing");
    audio->run(request(16U, "audio.pause", {{"id", JsonValue("music")}}));
    check(!flag(settle(16U), "ok"), "audio.pause of nothing fails");
    audio->run(request(
        17U, "audio.play",
        {{"source", JsonValue("assets/strata/media/absent.wav")}, {"muted", JsonValue(true)}}));
    check(!flag(settle(17U), "ok"), "a sound that is not there fails");
    audio->run(request(18U, "audio.play",
                       {{"source", JsonValue("assets/strata/media/chime.wav")},
                        {"id", JsonValue("chime")},
                        {"muted", JsonValue(true)}},
                       "gone"));
    audio->close_client("gone");
    audio.reset();
}

} // namespace

int main(const int argc, char** const argv) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::filesystem::path root;
    int status = 0;
    try {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = std::filesystem::path(temp) /
               (L"strata-platform-io-" + std::to_wstring(GetCurrentProcessId()));
        test_glob();
        test_base64();
        test_http_parts();
        test_paths();
        test_files(root);
        test_http();
        test_sockets();
        // Sound needs a device to play on, muted or not.
        if (argc > 1 && waveOutGetNumDevs() > 0U)
            test_audio(std::filesystem::path(wide(argv[1])));
        else
            std::cout << "platform io: no sound device, sound not tested\n";
        std::cout << "platform io tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "platform io test failed: " << error.what() << '\n';
        status = 1;
    }
    std::error_code ignored;
    if (!root.empty())
        std::filesystem::remove_all(root, ignored);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return status;
}
