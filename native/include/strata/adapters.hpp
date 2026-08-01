#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <strata/input.hpp>

namespace strata {

/** Resource provider retained by Runtime. A missing id returns std::nullopt. */
struct ResourceAdapter final {
    std::uint64_t generation = 1U;
    std::function<std::optional<std::vector<std::uint8_t>>(std::string_view)> load{};
};

struct DurableStoreAdapter final {
    std::function<std::optional<std::vector<std::uint8_t>>(std::string_view application_id)> load{};
    std::function<void(std::string_view application_id, std::span<const std::uint8_t> bytes)> write{};
};

struct AsyncRequest final {
    std::uint64_t id = 0U;
    std::string binding{};
    std::string owner{};
    std::string payload_json{};
};

struct AsyncHostAdapter final {
    std::function<void(const AsyncRequest&)> begin{};
    std::function<void(std::uint64_t request_id)> cancel{};
};

struct AsyncProgress final {
    double completed = 0.0;
    std::optional<double> total{};
    std::string message{};
};

struct ClipboardAdapter final {
    std::function<std::optional<std::string>()> read{};
    std::function<void(std::string_view)> write{};
};

struct ImeAdapter final {
    std::function<void(bool active)> set_active{};
    std::function<void(Rect logical_rect)> set_cursor_rect{};
};

struct EffectRequest final {
    std::string id{};
    std::string payload_json{};
};

struct EffectAdapter final {
    std::function<void(const EffectRequest&)> emit{};
};

} // namespace strata
