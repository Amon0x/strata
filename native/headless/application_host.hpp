#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <strata/strata.h>

#include "data/json.hpp"
#include "scenario.hpp"

namespace strata::headless {

struct CapturedDiagnostic final {
    std::uint64_t id = 0U;
    std::uint32_t severity = 0U;
    std::string code;
    std::string message;
    std::string source;
    std::string component_path;
    std::string expected;
};

struct CapturedAction final {
    std::string id;
    std::string payload;
    std::string event_kind;
    std::string source_key;
    std::string event_value;
};

struct CapturedEffect final {
    std::string id;
    std::string payload;
};

struct CapturedAsyncRequest final {
    std::uint64_t id = 0U;
    std::string binding;
    std::string owner;
    std::string payload;
    bool cancelled = false;
};

struct CapturedFrame final {
    std::uint64_t index = 0U;
    std::int64_t time = 0;
    std::uint64_t input_events = 0U;
    std::uint64_t emitted_events = 0U;
    std::uint64_t actions = 0U;
    std::uint64_t commands = 0U;
    std::uint64_t packet_bytes = 0U;
};

/** One complete public-ABI application/Surface host with deterministic in-memory services. */
class ApplicationHost final {
  public:
    ApplicationHost(const Scenario& scenario, std::filesystem::path resource_root);
    ~ApplicationHost();

    ApplicationHost(const ApplicationHost&) = delete;
    ApplicationHost& operator=(const ApplicationHost&) = delete;

    void frame(std::int64_t time_nanoseconds);
    void enqueue(std::span<const strata_input_event> events);
    void publish(const SnapshotConfig& snapshot);
    void resize(double width, double height, double scale, std::int64_t time_nanoseconds);
    void close();

    [[nodiscard]] bool has_frame() const noexcept;
    [[nodiscard]] std::string_view render_backend() const noexcept;
    [[nodiscard]] const std::string& frame_json() const noexcept;
    [[nodiscard]] const data::JsonValue& frame_document() const noexcept;
    [[nodiscard]] std::uint32_t framebuffer_width() const noexcept;
    [[nodiscard]] std::uint32_t framebuffer_height() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept;
    [[nodiscard]] const std::vector<CapturedDiagnostic>& diagnostics() const noexcept;
    [[nodiscard]] const std::vector<CapturedAction>& actions() const noexcept;
    [[nodiscard]] const std::vector<CapturedEffect>& effects() const noexcept;
    [[nodiscard]] const std::vector<CapturedAsyncRequest>& async_requests() const noexcept;
    [[nodiscard]] const std::vector<CapturedFrame>& frames() const noexcept;
    [[nodiscard]] const std::vector<std::string>& material_fallbacks() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::headless
