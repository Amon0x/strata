#include "platform_io.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
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
#include <Windows.h>
#include <winhttp.h>

namespace strata::desktop {
namespace {

using data::JsonValue;
using namespace platform_io;

/** The most a response or a WebSocket message may hold. */
constexpr std::size_t body_limit = 64U * 1024U * 1024U;
constexpr DWORD default_timeout_millis = 30'000U;
/** How long a closing WebSocket waits for the server's close before it hangs up. */
constexpr auto close_grace = std::chrono::seconds(5);

/** A failed WinHTTP call, with the error it left. */
class NetworkError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string error_text(const DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
        return "the request timed out";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "could not connect to the server";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "the server's name could not be resolved";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "the request was cancelled";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "the secure connection failed: the server's certificate was not accepted";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "the connection was reset";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return "the URL is not valid";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        return "the server's response was not valid HTTP";
    default:
        break;
    }
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"winhttp.dll"), code, 0U, reinterpret_cast<LPWSTR>(&text), 0U, nullptr);
    std::string result = length > 0U && text != nullptr ? utf8(std::wstring_view(text, length))
                                                        : "network error " + std::to_string(code);
    if (text != nullptr)
        LocalFree(text);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                               result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }
    return result;
}

[[noreturn]] void fail(const DWORD code) {
    throw NetworkError(error_text(code));
}

[[noreturn]] void fail_last() {
    fail(GetLastError());
}

[[nodiscard]] std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character + 32 : character);
    });
    return value;
}

/** Where a URL points. */
struct Target final {
    std::wstring host;
    INTERNET_PORT port = 0U;
    std::wstring path;
    bool secure = false;
};

/** Splits `url` (http or https; ws or wss for a WebSocket, which WinHTTP knows by the others). */
[[nodiscard]] Target target(const std::string_view url, const bool websocket) {
    std::string address(url);
    const std::string scheme = lower(address.substr(0U, address.find("://")));
    if (websocket) {
        if (scheme == "ws")
            address = "http" + address.substr(2U);
        else if (scheme == "wss")
            address = "https" + address.substr(3U);
        else
            throw NetworkError("a WebSocket URL starts with ws:// or wss://");
    } else if (scheme != "http" && scheme != "https") {
        throw NetworkError("an HTTP URL starts with http:// or https://");
    }
    const std::wstring wide_url = wide(address);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0U, &parts) ||
        parts.dwHostNameLength == 0U) {
        throw NetworkError("the URL is not valid: " + std::string(url));
    }
    Target result;
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    result.port = parts.nPort;
    if (parts.lpszUrlPath != nullptr)
        result.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr)
        result.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (result.path.empty())
        result.path = L"/";
    result.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return result;
}

/**
 * The WinHTTP handles of one exchange. Another thread cancels it by closing them, which ends any
 * call blocked on them; after that, a handle the exchange is given is closed at once.
 */
class Exchange final {
  public:
    explicit Exchange(std::string client) : client(std::move(client)) {}
    ~Exchange() {
        cancel();
    }

    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;

    /** Keeps `handle` to close later; false (having closed it) once the exchange is cancelled. */
    [[nodiscard]] bool adopt(const HINTERNET handle) {
        const std::scoped_lock lock(mutex_);
        if (cancelled_) {
            WinHttpCloseHandle(handle);
            return false;
        }
        handles_.push_back(handle);
        return true;
    }
    /** Closes one handle the exchange no longer needs. */
    void release(const HINTERNET handle) {
        const std::scoped_lock lock(mutex_);
        if (const auto found = std::ranges::find(handles_, handle); found != handles_.end()) {
            WinHttpCloseHandle(handle);
            handles_.erase(found);
        }
    }
    /** Closes every handle, newest first, ending whatever waits on them. */
    void cancel() {
        const std::scoped_lock lock(mutex_);
        cancelled_ = true;
        for (auto handle = handles_.rbegin(); handle != handles_.rend(); ++handle)
            WinHttpCloseHandle(*handle);
        handles_.clear();
    }
    [[nodiscard]] bool cancelled() {
        const std::scoped_lock lock(mutex_);
        return cancelled_;
    }

    const std::string client;

  private:
    std::mutex mutex_;
    std::vector<HINTERNET> handles_;
    bool cancelled_ = false;
};

/** Opens a connection and a request on `exchange`; nothing when it was cancelled meanwhile. */
[[nodiscard]] HINTERNET open_request(const HINTERNET session, Exchange& exchange, const Target& to,
                                     const std::wstring& method) {
    const HINTERNET connection = WinHttpConnect(session, to.host.c_str(), to.port, 0U);
    if (connection == nullptr)
        fail_last();
    if (!exchange.adopt(connection))
        return nullptr;
    const HINTERNET request =
        WinHttpOpenRequest(connection, method.c_str(), to.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, to.secure ? WINHTTP_FLAG_SECURE : 0U);
    if (request == nullptr)
        fail_last();
    if (!exchange.adopt(request))
        return nullptr;
    return request;
}

[[nodiscard]] std::wstring query_text(const HINTERNET request, const DWORD what) {
    DWORD size = 0U;
    WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0U)
        return {};
    std::wstring text(size / sizeof(wchar_t) + 1U, L'\0');
    if (!WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    text.resize(size / sizeof(wchar_t));
    return text;
}

[[nodiscard]] DWORD query_status(const HINTERNET request) {
    DWORD status = 0U;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

/** Sends `headers` (CRLF lines) and `body` on `request` and waits for the response's head. */
void send(const HINTERNET request, const std::string& headers, const std::string& body) {
    const std::wstring block = wide(headers);
    const DWORD size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request, block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : block.c_str(),
                            block.empty() ? 0U : static_cast<DWORD>(-1),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            size, size, 0U) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        fail_last();
    }
}

/** One WebSocket: a thread that connects and reads, and one that writes what is sent. */
struct Socket final {
    /** Something to send: a message, or the close that ends the socket. */
    struct Outgoing final {
        PlatformRequest request;
        std::string text;
        bool close = false;
        USHORT code = WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS;
        std::string reason;
    };

    Socket(PlatformRequest opened, std::string id)
        : opened(std::move(opened)), id(std::move(id)), exchange(this->opened.client) {}
    ~Socket() {
        stop();
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /** Says what happened on the socket's channel, unless it has been silenced. */
    void say(PlatformOutbox& outbox, JsonValue::Object data) {
        if (!silenced)
            outbox.say(opened, id, JsonValue(std::move(data)));
    }
    /** Hangs up at once without a word more: the socket is being replaced or its session left. */
    void abandon() {
        silenced = true;
        exchange.cancel();
        wake.notify_all();
    }
    void stop() {
        abandon();
        writer.request_stop();
        wake.notify_all();
        if (reader.joinable())
            reader.join();
        if (writer.joinable())
            writer.join();
    }

    const PlatformRequest opened;
    const std::string id;
    Exchange exchange;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<Outgoing> outgoing;
    HINTERNET handle = nullptr;
    bool open = false;
    std::atomic<bool> closing = false;
    std::atomic<bool> silenced = false;
    std::atomic<bool> finished = false;
    std::jthread writer;
    std::jthread reader;
};

class NetworkService final : public PlatformService {
  public:
    explicit NetworkService(PlatformOutbox& outbox)
        : outbox_(outbox), workers_(std::make_unique<PlatformWorkers>(4U)) {}

    ~NetworkService() override {
        for (auto& [key, socket] : sockets_)
            socket->abandon();
        for (const std::unique_ptr<Socket>& socket : retired_)
            socket->abandon();
        {
            const std::scoped_lock lock(mutex_);
            for (const std::shared_ptr<Exchange>& exchange : exchanges_)
                exchange->cancel();
        }
        // Workers and socket threads use the session: they end before it closes.
        workers_.reset();
        sockets_.clear();
        retired_.clear();
        if (session_ != nullptr)
            WinHttpCloseHandle(session_);
    }

    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;

    [[nodiscard]] bool serves(const std::string_view kind) const noexcept override {
        return kind.starts_with("http.") || kind.starts_with("socket.");
    }

    void run(const PlatformRequest& request) override {
        if (request.kind == "http.request")
            fetch(request);
        else if (request.kind == "socket.open")
            open_socket(request);
        else if (request.kind == "socket.send" || request.kind == "socket.close")
            queue(request);
        else
            outbox_.reply(request,
                          {{"error", JsonValue("unknown platform request " + request.kind)},
                           {"ok", JsonValue(false)}});
    }

    void tick() override {
        // Sockets whose threads have ended are let go; their joins are immediate.
        std::erase_if(retired_, [](const std::unique_ptr<Socket>& socket) {
            return socket->finished.load();
        });
        std::erase_if(sockets_, [](const auto& entry) { return entry.second->finished.load(); });
    }

    void close_client(const std::string_view client) override {
        {
            const std::scoped_lock lock(mutex_);
            for (const std::shared_ptr<Exchange>& exchange : exchanges_) {
                if (exchange->client == client)
                    exchange->cancel();
            }
        }
        for (auto entry = sockets_.begin(); entry != sockets_.end();) {
            if (entry->first.first == client) {
                entry->second->abandon();
                retired_.push_back(std::move(entry->second));
                entry = sockets_.erase(entry);
            } else {
                ++entry;
            }
        }
    }

  private:
    [[nodiscard]] HINTERNET session() {
        if (session_ != nullptr)
            return session_;
        // The system's proxy settings, as a browser would use them.
        session_ = WinHttpOpen(L"Strata", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0U);
        if (session_ == nullptr)
            session_ = WinHttpOpen(L"Strata", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0U);
        if (session_ == nullptr)
            fail_last();
        return session_;
    }

    // ---- HTTP ----

    void fetch(const PlatformRequest& request) {
        const HINTERNET from = session();
        auto exchange = std::make_shared<Exchange>(request.client);
        {
            const std::scoped_lock lock(mutex_);
            exchanges_.push_back(exchange);
        }
        workers_->submit([this, request, exchange, from] {
            JsonValue::Object answer;
            try {
                answer = transfer(from, request, *exchange);
            } catch (const std::exception& error) {
                answer = http_failure(error.what());
            }
            // A cancelled exchange belongs to a session that has gone: it hears nothing.
            if (!exchange->cancelled())
                outbox_.reply(request, std::move(answer));
            exchange->cancel();
            const std::scoped_lock lock(mutex_);
            std::erase(exchanges_, exchange);
        });
    }

    [[nodiscard]] static JsonValue::Object
    transfer(const HINTERNET session, const PlatformRequest& request, Exchange& exchange) {
        const auto started = std::chrono::steady_clock::now();
        const std::string url = text_field(request.payload, "url");
        std::string method = text_field(request.payload, "method", "GET");
        std::ranges::transform(method, method.begin(), [](const unsigned char character) {
            return static_cast<char>(character >= 'a' && character <= 'z' ? character - 32
                                                                          : character);
        });
        const std::optional<double> given = number_field(request.payload, "timeout");
        const DWORD timeout = given.has_value() && *given > 0.0
                                  ? static_cast<DWORD>(std::min(*given, 3'600'000.0))
                                  : default_timeout_millis;
        const std::string as = lower(text_field(request.payload, "as"));
        if (!as.empty() && as != "json" && as != "text")
            throw NetworkError("as is \"json\" or \"text\"");
        const HttpBody body = http_body(request.payload.find("body"));
        const std::string headers = http_header_block(request.payload.find("headers"), body);

        const HINTERNET handle = open_request(session, exchange, target(url, false), wide(method));
        if (handle == nullptr)
            return http_failure("the request was cancelled");
        // `timeout` bounds each step, and the whole exchange below.
        const int step = static_cast<int>(timeout);
        WinHttpSetTimeouts(handle, step, step, step, step);
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(handle, WINHTTP_OPTION_DECOMPRESSION, &decompression,
                         sizeof(decompression));
        send(handle, headers, body.bytes);

        const DWORD status = query_status(handle);
        std::string status_text = utf8(query_text(handle, WINHTTP_QUERY_STATUS_TEXT));
        JsonValue::Object response_headers =
            http_response_headers(utf8(query_text(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF)));
        std::string received;
        for (;;) {
            if (std::chrono::steady_clock::now() - started > std::chrono::milliseconds(timeout))
                fail(ERROR_WINHTTP_TIMEOUT);
            DWORD available = 0U;
            if (!WinHttpQueryDataAvailable(handle, &available))
                fail_last();
            if (available == 0U)
                break;
            if (received.size() + available > body_limit)
                throw NetworkError("the response is larger than 64 MB");
            const std::size_t at = received.size();
            received.resize(at + available);
            DWORD read = 0U;
            if (!WinHttpReadData(handle, received.data() + at, available, &read))
                fail_last();
            received.resize(at + read);
        }
        return http_answer(static_cast<std::int64_t>(status), std::move(status_text),
                           std::move(response_headers), std::move(received), as);
    }

    // ---- WebSockets ----

    void open_socket(const PlatformRequest& request) {
        const std::string id = text_field(request.payload, "id");
        if (id.empty())
            throw NetworkError("socket.open needs an id");
        const HINTERNET from = session();
        const auto key = std::make_pair(request.client, id);
        // Opening an id again replaces the socket that had it, which says nothing more.
        if (const auto found = sockets_.find(key); found != sockets_.end()) {
            found->second->abandon();
            retired_.push_back(std::move(found->second));
            sockets_.erase(found);
        }
        auto socket = std::make_unique<Socket>(request, id);
        Socket* const raw = socket.get();
        raw->writer = std::jthread([this, raw](const std::stop_token stop) { write(*raw, stop); });
        raw->reader = std::jthread([this, raw, from] { read(*raw, from); });
        sockets_.emplace(key, std::move(socket));
    }

    void queue(const PlatformRequest& request) {
        const std::string id = text_field(request.payload, "id");
        const auto found = sockets_.find(std::make_pair(request.client, id));
        if (found == sockets_.end() || found->second->finished) {
            outbox_.reply(request, {{"error", JsonValue("no socket is open as \"" + id + "\"")},
                                    {"ok", JsonValue(false)}});
            return;
        }
        Socket& socket = *found->second;
        Socket::Outgoing next{request, {}, request.kind == "socket.close"};
        if (next.close) {
            if (const std::optional<double> code = number_field(request.payload, "code");
                code.has_value() && *code >= 1000.0 && *code <= 4999.0) {
                next.code = static_cast<USHORT>(*code);
            }
            // A close reason is at most 123 bytes.
            next.reason = text_field(request.payload, "reason").substr(0U, 123U);
        } else if (const JsonValue* text = request.payload.find("text");
                   text != nullptr && text->string() != nullptr) {
            next.text = *text->string();
        } else if (const JsonValue* json = request.payload.find("json"); json != nullptr) {
            next.text = compact_json(*json);
        } else if (const JsonValue* event = request.payload.find("event");
                   event != nullptr && event->string() != nullptr) {
            // What a text box submitted, say.
            next.text = *event->string();
        } else {
            outbox_.reply(request, {{"error", JsonValue("socket.send needs text or json")},
                                    {"ok", JsonValue(false)}});
            return;
        }
        {
            const std::scoped_lock lock(socket.mutex);
            socket.outgoing.push_back(std::move(next));
        }
        socket.wake.notify_all();
    }

    /** Connects, then hands each message and the close to the socket's channel until it ends. */
    void read(Socket& socket, const HINTERNET session) {
        try {
            connect(socket, session);
            receive(socket);
        } catch (const std::exception& error) {
            if (!socket.silenced) {
                outbox_.reply(socket.opened, {{"error", JsonValue(std::string(error.what()))},
                                              {"id", JsonValue(socket.id)},
                                              {"ok", JsonValue(false)}});
                socket.say(outbox_, {{"error", JsonValue(std::string(error.what()))},
                                     {"type", JsonValue("error")}});
            }
        }
        {
            const std::scoped_lock lock(socket.mutex);
            socket.open = false;
        }
        socket.finished = true;
        socket.wake.notify_all();
    }

    void connect(Socket& socket, const HINTERNET session) {
        const HINTERNET request =
            open_request(session, socket.exchange,
                         target(text_field(socket.opened.payload, "url"), true), L"GET");
        if (request == nullptr)
            throw NetworkError("the socket was closed");
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0U))
            fail_last();
        const int step = static_cast<int>(default_timeout_millis);
        WinHttpSetTimeouts(request, step, step, step, step);
        std::string headers = http_header_block(socket.opened.payload.find("headers"), {});
        std::string protocols;
        if (const JsonValue* given = socket.opened.payload.find("protocols"); given != nullptr) {
            if (given->string() != nullptr) {
                protocols = *given->string();
            } else if (given->array() != nullptr) {
                for (const JsonValue& protocol : *given->array()) {
                    if (protocol.string() == nullptr)
                        continue;
                    if (!protocols.empty())
                        protocols += ", ";
                    protocols += *protocol.string();
                }
            }
        }
        if (!protocols.empty())
            headers += "Sec-WebSocket-Protocol: " + protocols + "\r\n";
        send(request, headers, {});
        if (const DWORD status = query_status(request); status != 101U) {
            throw NetworkError("the server did not accept the WebSocket (HTTP " +
                               std::to_string(status) + ")");
        }
        const JsonValue::Object response =
            http_response_headers(utf8(query_text(request, WINHTTP_QUERY_RAW_HEADERS_CRLF)));
        const auto accepted = std::ranges::find(response, std::string("sec-websocket-protocol"),
                                                &JsonValue::ObjectEntry::first);
        const std::string protocol = accepted != response.end() ? *accepted->second.string() : "";
        // An open socket waits as long as it takes for its next message.
        DWORD forever = 0U;
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof(forever));
        const HINTERNET handle = WinHttpWebSocketCompleteUpgrade(request, 0U);
        if (handle == nullptr)
            fail_last();
        if (!socket.exchange.adopt(handle))
            throw NetworkError("the socket was closed");
        socket.exchange.release(request);
        {
            const std::scoped_lock lock(socket.mutex);
            socket.handle = handle;
            socket.open = true;
        }
        socket.wake.notify_all();
        if (!socket.silenced) {
            outbox_.reply(socket.opened, {{"id", JsonValue(socket.id)},
                                          {"ok", JsonValue(true)},
                                          {"protocol", JsonValue(protocol)}});
        }
        socket.say(outbox_, {{"protocol", JsonValue(protocol)}, {"type", JsonValue("open")}});
    }

    void receive(Socket& socket) {
        std::vector<std::uint8_t> buffer(64U * 1024U);
        std::string message;
        for (;;) {
            DWORD read = 0U;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD error = WinHttpWebSocketReceive(
                socket.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
            if (error != NO_ERROR) {
                // Hung up on: by this side after a close the server never answered, or by the
                // network.
                if (!socket.closing)
                    socket.say(outbox_, {{"error", JsonValue(error_text(error))},
                                         {"type", JsonValue("error")}});
                socket.say(outbox_, {{"code", JsonValue(std::int64_t{1006})},
                                     {"reason", JsonValue("")},
                                     {"type", JsonValue("closed")}});
                return;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT code = 0U;
                std::array<char, 128> reason{};
                DWORD length = 0U;
                WinHttpWebSocketQueryCloseStatus(socket.handle, &code, reason.data(),
                                                 static_cast<DWORD>(reason.size()), &length);
                // The server began the close: answer it, as the protocol asks.
                if (!socket.closing.exchange(true))
                    WinHttpWebSocketClose(socket.handle, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                          nullptr, 0U);
                socket.say(outbox_, {{"code", JsonValue(static_cast<std::int64_t>(code))},
                                     {"reason", JsonValue(std::string(reason.data(), length))},
                                     {"type", JsonValue("closed")}});
                return;
            }
            if (message.size() + read > body_limit) {
                socket.say(outbox_, {{"error", JsonValue("a message was larger than 64 MB")},
                                     {"type", JsonValue("error")}});
                message.clear();
                continue;
            }
            message.append(reinterpret_cast<const char*>(buffer.data()), read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                if (!socket.silenced)
                    outbox_.say(socket.opened, socket.id, socket_message(std::move(message)));
                message.clear();
            } else if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                const auto* const bytes = reinterpret_cast<const std::uint8_t*>(message.data());
                socket.say(outbox_, {{"base64", JsonValue(base64_encode({bytes, message.size()}))},
                                     {"type", JsonValue("message")}});
                message.clear();
            }
        }
    }

    /** Sends what is queued, in order, once the socket is open; a close ends it. */
    void write(Socket& socket, const std::stop_token stop) {
        std::unique_lock lock(socket.mutex);
        while (!stop.stop_requested()) {
            const bool due = socket.wake.wait(lock, stop, [&socket] {
                return socket.finished || (socket.open && !socket.outgoing.empty());
            });
            if (!due || socket.finished)
                break;
            Socket::Outgoing next = std::move(socket.outgoing.front());
            socket.outgoing.pop_front();
            const HINTERNET handle = socket.handle;
            lock.unlock();
            if (next.close) {
                socket.closing = true;
                const DWORD error = WinHttpWebSocketShutdown(
                    handle, next.code,
                    next.reason.empty() ? nullptr : const_cast<char*>(next.reason.data()),
                    static_cast<DWORD>(next.reason.size()));
                answer(next.request, error);
                // The reader ends once the server closes too, or is hung up on.
                lock.lock();
                if (!socket.wake.wait_for(lock, stop, close_grace,
                                          [&socket] { return socket.finished.load(); })) {
                    lock.unlock();
                    socket.exchange.cancel();
                    lock.lock();
                }
                break;
            }
            const DWORD error =
                WinHttpWebSocketSend(handle, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     next.text.empty() ? nullptr : next.text.data(),
                                     static_cast<DWORD>(next.text.size()));
            answer(next.request, error);
            lock.lock();
        }
        // What was never sent fails.
        std::deque<Socket::Outgoing> unsent = std::move(socket.outgoing);
        socket.outgoing.clear();
        lock.unlock();
        for (const Socket::Outgoing& left : unsent) {
            outbox_.reply(left.request,
                          {{"error", JsonValue("the socket is closed")}, {"ok", JsonValue(false)}});
        }
    }

    void answer(const PlatformRequest& request, const DWORD error) {
        if (error == NO_ERROR)
            outbox_.reply(request, {{"ok", JsonValue(true)}});
        else
            outbox_.reply(request,
                          {{"error", JsonValue(error_text(error))}, {"ok", JsonValue(false)}});
    }

    PlatformOutbox& outbox_;
    HINTERNET session_ = nullptr;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Exchange>> exchanges_;
    std::map<std::pair<std::string, std::string>, std::unique_ptr<Socket>> sockets_;
    std::vector<std::unique_ptr<Socket>> retired_;
    std::unique_ptr<PlatformWorkers> workers_;
};

} // namespace

std::unique_ptr<PlatformService> make_network_service(PlatformOutbox& outbox) {
    return std::make_unique<NetworkService>(outbox);
}

} // namespace strata::desktop
