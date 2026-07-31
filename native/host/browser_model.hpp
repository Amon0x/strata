#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "data/json.hpp"

namespace strata::host {

struct Selector final {
    std::optional<std::string> path;
    std::optional<std::string> key;
    std::optional<std::string> name;
    std::optional<std::string> role;
    std::optional<double> x;
    std::optional<double> y;
};

struct BrowserBounds final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct BrowserElement final {
    std::string path;
    std::string key;
    std::string name;
    std::string role;
    data::JsonValue actions;
    data::JsonValue state;
    std::optional<BrowserBounds> hit_bounds;
    bool virtual_node = false;
};

/** Per-frame semantic tree joined with exact retained and presenter-owned pointer geometry. */
class BrowserModel final {
  public:
    [[nodiscard]] static BrowserModel build(const data::JsonValue& frame, double viewport_width,
                                            double viewport_height);

    [[nodiscard]] std::pair<double, double> resolve(const Selector& selector) const;
    [[nodiscard]] data::JsonValue document() const;
    [[nodiscard]] const std::vector<BrowserElement>& elements() const noexcept;

  private:
    std::vector<BrowserElement> elements_;
};

} // namespace strata::host
