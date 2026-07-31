#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "data/json.hpp"
#include "host/browser_model.hpp"

namespace strata::headless {

struct FontConfig final {
    std::string id;
    std::string resource;
};

struct TextureConfig final {
    std::string id;
    std::string resource;
    std::uint32_t sampling = 1U;
};

struct SnapshotConfig final {
    std::string id;
    data::JsonValue values;
};

using Selector = host::Selector;

struct AdvanceStep final {
    std::int64_t duration_nanoseconds = 0;
    std::uint32_t frames = 1U;
};
struct CaptureStep final {
    std::string name;
};
struct ClickStep final {
    Selector target;
};
struct MoveStep final {
    Selector target;
};
struct ScrollStep final {
    Selector target;
    double delta_x = 0.0;
    double delta_y = 0.0;
};
struct KeyStep final {
    std::string key;
    std::uint32_t modifiers = 0U;
};
struct TextStep final {
    std::string text;
};
struct ResizeStep final {
    double width = 0.0;
    double height = 0.0;
    double scale = 1.0;
};
struct PublishStep final {
    SnapshotConfig snapshot;
};

using ScenarioStep = std::variant<AdvanceStep, CaptureStep, ClickStep, MoveStep, ScrollStep,
                                  KeyStep, TextStep, ResizeStep, PublishStep>;

struct Scenario final {
    std::uint32_t version = 1U;
    std::string application_id;
    std::string surface_id;
    std::filesystem::path module;
    std::filesystem::path schemas;
    std::string root;
    std::string root_role = "overlay";
#if defined(_WIN32)
    std::string render_backend = "d3d11";
#else
    std::string render_backend = "reference";
#endif
    std::vector<std::string> packages;
    std::vector<std::string> actions;
    std::vector<FontConfig> fonts;
    std::vector<TextureConfig> textures;
    std::vector<SnapshotConfig> snapshots;
    std::vector<ScenarioStep> steps;
    double width = 1280.0;
    double height = 800.0;
    double scale = 1.0;
    bool reduced_motion = false;
    std::array<std::uint8_t, 4U> clear_color{9U, 11U, 15U, 255U};
};

[[nodiscard]] Scenario load_scenario(const std::filesystem::path& path);
/** Parses one strict scenario operation for batch or interactive execution. */
[[nodiscard]] ScenarioStep parse_scenario_step(const data::JsonValue& value,
                                               std::size_t index = 0U);

} // namespace strata::headless
