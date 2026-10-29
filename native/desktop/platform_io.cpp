#include "platform_io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <exception>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>

namespace strata::desktop {

using data::JsonValue;

PlatformWorkers::PlatformWorkers(const std::size_t limit)
    : limit_(std::max<std::size_t>(limit, 1U)) {}

PlatformWorkers::~PlatformWorkers() {
    {
        const std::scoped_lock lock(mutex_);
        tasks_.clear();
    }
    for (std::jthread& thread : threads_)
        thread.request_stop();
    ready_.notify_all();
    threads_.clear();
}

void PlatformWorkers::submit(std::function<void()> task) {
    {
        const std::scoped_lock lock(mutex_);
        tasks_.push_back(std::move(task));
        // A new thread only when every one there is busy.
        if (idle_ < tasks_.size() && threads_.size() < limit_)
            threads_.emplace_back([this](const std::stop_token stop) { work(stop); });
    }
    ready_.notify_one();
}

void PlatformWorkers::work(const std::stop_token stop) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        ++idle_;
        const bool woke = ready_.wait(lock, stop, [this] { return !tasks_.empty(); });
        --idle_;
        if (!woke)
            break;
        std::function<void()> task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        try {
            task();
        } catch (...) {
            // A task answers its own failures; nothing may end the worker.
        }
        lock.lock();
    }
    lock.unlock();
    if (SUCCEEDED(apartment))
        CoUninitialize();
}

namespace platform_io {

std::string utf8(const std::wstring_view value) {
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                            size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string_view value) {
    if (value.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                            size);
    return result;
}

std::string text_field(const JsonValue& payload, const std::string_view name,
                       std::string fallback) {
    const JsonValue* value = payload.find(name);
    return value != nullptr && value->string() != nullptr ? *value->string() : std::move(fallback);
}

bool flag_field(const JsonValue& payload, const std::string_view name, const bool fallback) {
    const JsonValue* value = payload.find(name);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

std::optional<double> number_field(const JsonValue& payload, const std::string_view name) {
    const JsonValue* value = payload.find(name);
    if (value == nullptr)
        return std::nullopt;
    if (value->number() != nullptr && std::isfinite(*value->number()))
        return *value->number();
    if (value->integer() != nullptr)
        return static_cast<double>(*value->integer());
    return std::nullopt;
}

std::string requested_path(const JsonValue& payload, const std::string_view field) {
    if (std::string named = text_field(payload, field); !named.empty())
        return named;
    const JsonValue* event = payload.find("event");
    if (event == nullptr)
        return {};
    if (event->string() != nullptr)
        return *event->string();
    if (const JsonValue* path = event->find("path"); path != nullptr && path->string() != nullptr)
        return *path->string();
    if (const JsonValue* files = event->find("files");
        files != nullptr && files->array() != nullptr && !files->array()->empty() &&
        files->array()->front().string() != nullptr) {
        return *files->array()->front().string();
    }
    return {};
}

std::filesystem::path resolve_path(const std::string_view path,
                                   const ResourcePathResolver& resolve) {
    std::wstring value = wide(path);
    if (value.find(L'%') != std::wstring::npos) {
        const DWORD size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0U);
        if (size > 0U) {
            std::wstring expanded(size, L'\0');
            const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), size);
            if (written > 0U && written <= size) {
                expanded.resize(written - 1U);
                value = std::move(expanded);
            }
        }
    }
    // A URL stays as it is written, slashes and all.
    if (value.find(L"://") != std::wstring::npos)
        return std::filesystem::path(value);
    const bool absolute = (value.size() >= 2U && value[1] == L':') || value.starts_with(L"\\\\") ||
                          value.starts_with(L"//");
    if (absolute || !resolve)
        return std::filesystem::path(value).make_preferred();
    return std::filesystem::path(resolve(utf8(value))).make_preferred();
}

bool glob_match(const std::wstring_view pattern, const std::wstring_view name) {
    const auto fold = [](const wchar_t character) {
        return character < 0x80 ? static_cast<wchar_t>(std::towlower(character)) : character;
    };
    // Greedy with one backtrack point: the last star, which may swallow one more character.
    std::size_t p = 0U;
    std::size_t n = 0U;
    std::optional<std::size_t> star;
    std::size_t resume = 0U;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            resume = n;
        } else if (p < pattern.size() &&
                   (pattern[p] == L'?' || fold(pattern[p]) == fold(name[n]))) {
            ++p;
            ++n;
        } else if (star.has_value()) {
            p = *star + 1U;
            n = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*')
        ++p;
    return p == pattern.size();
}

namespace {

constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string base64_encode(const std::span<const std::uint8_t> bytes) {
    std::string result;
    result.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t index = 0U; index < bytes.size(); index += 3U) {
        const std::size_t left = std::min<std::size_t>(3U, bytes.size() - index);
        std::uint32_t group = static_cast<std::uint32_t>(bytes[index]) << 16U;
        if (left > 1U)
            group |= static_cast<std::uint32_t>(bytes[index + 1U]) << 8U;
        if (left > 2U)
            group |= bytes[index + 2U];
        result += base64_alphabet[(group >> 18U) & 63U];
        result += base64_alphabet[(group >> 12U) & 63U];
        result += left > 1U ? base64_alphabet[(group >> 6U) & 63U] : '=';
        result += left > 2U ? base64_alphabet[group & 63U] : '=';
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> base64_decode(const std::string_view text) {
    std::vector<std::uint8_t> result;
    result.reserve(text.size() / 4U * 3U);
    std::uint32_t group = 0U;
    int bits = 0;
    bool padded = false;
    for (const char character : text) {
        if (character == ' ' || character == '\n' || character == '\r' || character == '\t')
            continue;
        if (character == '=') {
            padded = true;
            continue;
        }
        // Text after padding is not base64; the URL-safe alphabet is.
        if (padded)
            return std::nullopt;
        int value = -1;
        if (character == '-')
            value = 62;
        else if (character == '_')
            value = 63;
        else if (const std::size_t found = base64_alphabet.find(character);
                 found != std::string_view::npos)
            value = static_cast<int>(found);
        if (value < 0)
            return std::nullopt;
        group = (group << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(
                static_cast<std::uint8_t>((group >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    // A lone sextet left over cannot have come from whole bytes.
    if (bits >= 6)
        return std::nullopt;
    return result;
}

std::string compact_json(const JsonValue& value) {
    std::string line = data::encode_json_line(value);
    if (!line.empty() && line.back() == '\n')
        line.pop_back();
    return line;
}

double unix_millis(const std::uint64_t file_time) noexcept {
    constexpr std::uint64_t epoch = 116'444'736'000'000'000ULL;
    if (file_time < epoch)
        return 0.0;
    return static_cast<double>((file_time - epoch) / 10'000ULL);
}

HttpBody http_body(const JsonValue* const body) {
    if (body == nullptr || body->is_null())
        return {};
    if (body->string() != nullptr)
        return HttpBody{*body->string(), "text/plain; charset=utf-8"};
    return HttpBody{compact_json(*body), "application/json; charset=utf-8"};
}

namespace {

[[nodiscard]] std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] std::string_view trimmed(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1U);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1U);
    return value;
}

/** A header value as text: no line breaks, which would start a header of their own. */
[[nodiscard]] std::optional<std::string> header_text(const JsonValue& value) {
    std::string text;
    if (value.string() != nullptr)
        text = *value.string();
    else if (value.integer() != nullptr)
        text = std::to_string(*value.integer());
    else if (value.number() != nullptr || value.boolean() != nullptr)
        text = compact_json(value);
    else
        return std::nullopt;
    if (text.find_first_of("\r\n") != std::string::npos)
        return std::nullopt;
    return text;
}

} // namespace

std::string http_header_block(const JsonValue* const headers, const HttpBody& body) {
    std::string block;
    bool typed = false;
    if (headers != nullptr && headers->object() != nullptr) {
        for (const auto& [name, value] : *headers->object()) {
            const std::optional<std::string> text = header_text(value);
            if (name.empty() || name.find_first_of(":\r\n ") != std::string::npos ||
                !text.has_value()) {
                continue;
            }
            typed = typed || lower(name) == "content-type";
            block += name + ": " + *text + "\r\n";
        }
    }
    if (!typed && !body.content_type.empty())
        block += "Content-Type: " + body.content_type + "\r\n";
    return block;
}

JsonValue::Object http_response_headers(const std::string_view raw) {
    JsonValue::Object headers;
    std::size_t start = raw.find('\n');
    // The first line is the status line.
    start = start == std::string_view::npos ? raw.size() : start + 1U;
    while (start < raw.size()) {
        std::size_t end = raw.find('\n', start);
        if (end == std::string_view::npos)
            end = raw.size();
        const std::string_view line = raw.substr(start, end - start);
        start = end + 1U;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U)
            continue;
        const std::string name = lower(std::string(trimmed(line.substr(0U, colon))));
        const std::string value(trimmed(line.substr(colon + 1U)));
        const auto found = std::ranges::find(headers, name, &JsonValue::ObjectEntry::first);
        if (found == headers.end()) {
            headers.emplace_back(name, JsonValue(value));
        } else {
            found->second = JsonValue(*found->second.string() + ", " + value);
        }
    }
    std::ranges::sort(headers, {}, &JsonValue::ObjectEntry::first);
    return headers;
}

JsonValue::Object http_answer(const std::int64_t status, std::string status_text,
                              JsonValue::Object headers, std::string body,
                              const std::string_view as) {
    bool json = as == "json";
    if (as.empty()) {
        const auto type =
            std::ranges::find(headers, std::string("content-type"), &JsonValue::ObjectEntry::first);
        json = type != headers.end() && type->second.string() != nullptr &&
               type->second.string()->find("json") != std::string::npos;
    }
    JsonValue::Object answer{
        {"headers", JsonValue(std::move(headers))},
        {"ok", JsonValue(status >= 200 && status < 300)},
        {"status", JsonValue(status)},
        {"statusText", JsonValue(std::move(status_text))},
    };
    if (json) {
        try {
            answer.emplace_back("json", data::parse_json(body));
            return answer;
        } catch (const std::exception& error) {
            answer[1].second = JsonValue(false);
            answer.emplace_back(
                "error", JsonValue("the response is not JSON: " + std::string(error.what())));
        }
    }
    answer.emplace_back("body", JsonValue(std::move(body)));
    return answer;
}

JsonValue::Object http_failure(std::string error) {
    return {
        {"body", JsonValue("")},
        {"error", JsonValue(std::move(error))},
        {"headers", JsonValue(JsonValue::Object{})},
        {"ok", JsonValue(false)},
        {"status", JsonValue(std::int64_t{0})},
        {"statusText", JsonValue("")},
    };
}

JsonValue socket_message(std::string text) {
    JsonValue::Object message{{"type", JsonValue("message")}};
    try {
        JsonValue parsed = data::parse_json(text);
        message.emplace_back("json", std::move(parsed));
    } catch (const std::exception&) {
        // Plain text: no json.
    }
    message.emplace_back("text", JsonValue(std::move(text)));
    return JsonValue(std::move(message));
}

} // namespace platform_io

} // namespace strata::desktop
