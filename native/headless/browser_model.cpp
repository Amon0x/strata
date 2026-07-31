#include "browser_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::headless {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] std::optional<double> number(const JsonValue* value) {
    if (value == nullptr)
        return std::nullopt;
    if (const double* result = value->number(); result != nullptr)
        return *result;
    if (const std::int64_t* result = value->integer(); result != nullptr) {
        return static_cast<double>(*result);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<BrowserBounds> bounds(const JsonValue* value) {
    if (value == nullptr || value->object() == nullptr)
        return std::nullopt;
    const std::optional<double> x = number(value->find("x"));
    const std::optional<double> y = number(value->find("y"));
    const std::optional<double> width = number(value->find("width"));
    const std::optional<double> height = number(value->find("height"));
    if (!x.has_value() || !y.has_value() || !width.has_value() || !height.has_value() ||
        !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*width) ||
        !std::isfinite(*height) || *width <= 0.0 || *height <= 0.0) {
        return std::nullopt;
    }
    return BrowserBounds{*x, *y, *width, *height};
}

[[nodiscard]] std::optional<BrowserBounds> intersection(const BrowserBounds& left,
                                                        const BrowserBounds& right) {
    const double x = std::max(left.x, right.x);
    const double y = std::max(left.y, right.y);
    const double right_edge = std::min(left.x + left.width, right.x + right.width);
    const double bottom_edge = std::min(left.y + left.height, right.y + right.height);
    if (right_edge <= x || bottom_edge <= y)
        return std::nullopt;
    return BrowserBounds{x, y, right_edge - x, bottom_edge - y};
}

[[nodiscard]] BrowserBounds joined(const BrowserBounds& left, const BrowserBounds& right) {
    const double x = std::min(left.x, right.x);
    const double y = std::min(left.y, right.y);
    const double right_edge = std::max(left.x + left.width, right.x + right.width);
    const double bottom_edge = std::max(left.y + left.height, right.y + right.height);
    return BrowserBounds{x, y, right_edge - x, bottom_edge - y};
}

[[nodiscard]] std::string text(const JsonValue* value) {
    return value != nullptr && value->string() != nullptr ? *value->string() : std::string{};
}

[[nodiscard]] std::optional<std::int64_t> integer(const JsonValue* value) {
    return value != nullptr && value->integer() != nullptr
               ? std::optional<std::int64_t>(*value->integer())
               : std::nullopt;
}

[[nodiscard]] bool boolean(const JsonValue* value, const bool fallback = false) {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

struct Subtarget final {
    BrowserBounds bounds;
    std::string command_id;
    std::string id;
    std::string label;
    std::vector<std::size_t> path;
    std::optional<std::uint64_t> notification_id;
    std::size_t index = 0U;
    bool enabled = true;
};

struct InspectionGeometry final {
    std::optional<BrowserBounds> hit_bounds;
    std::vector<Subtarget> subtargets;
};

using GeometryIndex = std::map<std::string, InspectionGeometry, std::less<>>;

[[nodiscard]] std::vector<std::size_t> index_path(const JsonValue* value) {
    std::vector<std::size_t> result;
    if (value == nullptr || value->array() == nullptr)
        return result;
    for (const JsonValue& entry : *value->array()) {
        if (entry.integer() == nullptr || *entry.integer() < 0)
            return {};
        result.push_back(static_cast<std::size_t>(*entry.integer()));
    }
    return result;
}

void collect_geometry(const JsonValue& node, const BrowserBounds& viewport,
                      const std::optional<BrowserBounds>& inherited_clip, GeometryIndex& output) {
    if (node.object() == nullptr)
        return;
    std::optional<BrowserBounds> effective_clip = inherited_clip;
    if (const std::optional<BrowserBounds> local = bounds(node.find("clip")); local.has_value()) {
        effective_clip = effective_clip.has_value() ? intersection(*effective_clip, *local) : local;
    }

    const std::string path = text(node.find("structuralPath"));
    if (!path.empty()) {
        InspectionGeometry geometry;
        if (const std::optional<BrowserBounds> raw = bounds(node.find("hitBounds"));
            raw.has_value() && effective_clip.has_value()) {
            geometry.hit_bounds = intersection(*raw, *effective_clip);
        }
        const JsonValue* subtargets = node.find("subtargets");
        if (subtargets != nullptr && subtargets->array() != nullptr) {
            for (const JsonValue& value : *subtargets->array()) {
                const std::optional<BrowserBounds> raw = bounds(value.find("bounds"));
                if (!raw.has_value())
                    continue;
                const bool detached = boolean(value.find("detached"));
                const std::optional<BrowserBounds> clip =
                    detached ? std::optional(viewport) : effective_clip;
                const std::optional<BrowserBounds> clipped =
                    clip.has_value() ? intersection(*raw, *clip) : std::nullopt;
                if (!clipped.has_value())
                    continue;
                const std::optional<std::int64_t> parsed_index = integer(value.find("index"));
                if (!parsed_index.has_value() || *parsed_index < 0)
                    continue;
                Subtarget target{
                    *clipped,
                    text(value.find("commandId")),
                    text(value.find("id")),
                    text(value.find("label")),
                    index_path(value.find("path")),
                    std::nullopt,
                    static_cast<std::size_t>(*parsed_index),
                    boolean(value.find("enabled"), true),
                };
                if (const std::optional<std::int64_t> notification =
                        integer(value.find("notificationId"));
                    notification.has_value() && *notification >= 0) {
                    target.notification_id = static_cast<std::uint64_t>(*notification);
                }
                geometry.subtargets.push_back(std::move(target));
            }
        }
        output.insert_or_assign(path, std::move(geometry));
    }

    const JsonValue* children = node.find("children");
    if (children == nullptr || children->array() == nullptr)
        return;
    for (const JsonValue& child : *children->array()) {
        collect_geometry(child, viewport, effective_clip, output);
    }
}

[[nodiscard]] const InspectionGeometry* owner_geometry(const GeometryIndex& geometry,
                                                       const std::string_view path,
                                                       std::string& owner_path) {
    const InspectionGeometry* result = nullptr;
    for (const auto& [candidate, value] : geometry) {
        if (candidate.size() > path.size() || path.substr(0U, candidate.size()) != candidate ||
            (candidate != "/" && candidate.size() != path.size() &&
             path[candidate.size()] != '/')) {
            continue;
        }
        if (candidate.size() >= owner_path.size()) {
            owner_path = candidate;
            result = &value;
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::size_t> semantic_virtual_path(const std::string_view owner,
                                                             const std::string_view semantic) {
    if (semantic.size() <= owner.size())
        return {};
    std::vector<std::size_t> result;
    std::size_t offset = owner == "/" ? 1U : owner.size() + 1U;
    while (offset < semantic.size()) {
        const std::size_t end = semantic.find('/', offset);
        const std::string_view component = semantic.substr(
            offset, end == std::string_view::npos ? semantic.size() - offset : end - offset);
        std::uint64_t parsed = 0U;
        for (const char character : component) {
            if (character < '0' || character > '9')
                return {};
            parsed = parsed * 10U + static_cast<std::uint64_t>(character - '0');
            if (parsed > std::numeric_limits<std::size_t>::max())
                return {};
        }
        if (result.empty()) {
            if (parsed >= 2'100'000U)
                parsed -= 2'100'000U;
            else if (parsed >= 2'000'000U)
                parsed -= 2'000'000U;
            else if (parsed >= 1'000'000U)
                parsed -= 1'000'000U;
        }
        result.push_back(static_cast<std::size_t>(parsed));
        if (end == std::string_view::npos)
            break;
        offset = end + 1U;
    }
    return result;
}

[[nodiscard]] std::optional<BrowserBounds> virtual_bounds(const JsonValue& semantic,
                                                          const InspectionGeometry& owner,
                                                          const std::string_view owner_path,
                                                          const std::string_view semantic_path) {
    const std::optional<std::int64_t> virtual_index = integer(semantic.find("virtualIndex"));
    const std::optional<std::int64_t> notification =
        integer(semantic.find("virtualNotificationId"));
    const std::string command = text(semantic.find("virtualCommandId"));
    const std::string name = text(semantic.find("name"));
    const std::vector<std::size_t> path = semantic_virtual_path(owner_path, semantic_path);

    std::vector<const Subtarget*> candidates;
    for (const Subtarget& target : owner.subtargets) {
        if (!command.empty() && target.command_id != command)
            continue;
        if (notification.has_value() &&
            (!target.notification_id.has_value() ||
             *target.notification_id != static_cast<std::uint64_t>(*notification))) {
            continue;
        }
        if (virtual_index.has_value() &&
            (*virtual_index < 0 || target.index != static_cast<std::size_t>(*virtual_index))) {
            continue;
        }
        if (!target.path.empty() && !path.empty() && target.path != path)
            continue;
        candidates.push_back(&target);
    }
    if (candidates.empty() && virtual_index.has_value())
        return std::nullopt;
    if (candidates.empty() && !name.empty()) {
        for (const Subtarget& target : owner.subtargets) {
            if (target.label == name || target.id == name)
                candidates.push_back(&target);
        }
    }
    if (candidates.size() > 1U && !name.empty()) {
        std::vector<const Subtarget*> named;
        for (const Subtarget* target : candidates) {
            if (target->label == name || target->id == name)
                named.push_back(target);
        }
        if (!named.empty())
            candidates = std::move(named);
    }
    std::optional<BrowserBounds> result;
    for (const Subtarget* target : candidates) {
        result = result.has_value() ? std::optional(joined(*result, target->bounds))
                                    : std::optional(target->bounds);
    }
    return result;
}

void collect_semantics(const JsonValue& semantic, const GeometryIndex& geometry,
                       std::vector<BrowserElement>& output) {
    if (semantic.object() == nullptr)
        return;
    const std::string path = text(semantic.find("structuralPath"));
    std::string owner_path;
    const InspectionGeometry* owner = owner_geometry(geometry, path, owner_path);
    const bool virtual_node = owner != nullptr && owner_path != path;
    std::optional<BrowserBounds> hit_bounds;
    if (owner != nullptr) {
        hit_bounds =
            virtual_node ? virtual_bounds(semantic, *owner, owner_path, path) : owner->hit_bounds;
    }
    output.push_back(BrowserElement{
        path,
        text(semantic.find("key")),
        text(semantic.find("name")),
        text(semantic.find("role")),
        semantic.find("actions") != nullptr ? *semantic.find("actions") : array(),
        semantic.find("state") != nullptr ? *semantic.find("state") : object({}),
        hit_bounds,
        virtual_node,
    });
    const JsonValue* children = semantic.find("children");
    if (children == nullptr || children->array() == nullptr)
        return;
    for (const JsonValue& child : *children->array()) {
        collect_semantics(child, geometry, output);
    }
}

[[nodiscard]] JsonValue encoded_bounds(const std::optional<BrowserBounds>& value) {
    if (!value.has_value())
        return JsonValue{};
    return object({
        {"height", JsonValue(value->height)},
        {"width", JsonValue(value->width)},
        {"x", JsonValue(value->x)},
        {"y", JsonValue(value->y)},
    });
}

[[nodiscard]] bool has_actions(const JsonValue& actions) {
    return actions.array() != nullptr && !actions.array()->empty();
}

} // namespace

BrowserModel BrowserModel::build(const JsonValue& frame, const double viewport_width,
                                 const double viewport_height) {
    if (!std::isfinite(viewport_width) || !std::isfinite(viewport_height) ||
        viewport_width <= 0.0 || viewport_height <= 0.0) {
        throw std::invalid_argument("browser viewport must be positive and finite");
    }
    const BrowserBounds viewport{0.0, 0.0, viewport_width, viewport_height};
    const JsonValue* inspection = frame.find("inspection");
    const JsonValue* inspection_root = inspection != nullptr ? inspection->find("root") : nullptr;
    const JsonValue* semantics = frame.find("semantics");
    const JsonValue* semantics_root = semantics != nullptr ? semantics->find("root") : nullptr;
    if (inspection_root == nullptr || inspection_root->object() == nullptr ||
        semantics_root == nullptr || semantics_root->object() == nullptr) {
        throw std::runtime_error("headless frame does not contain inspectable UI roots");
    }

    GeometryIndex geometry;
    collect_geometry(*inspection_root, viewport, viewport, geometry);
    BrowserModel result;
    collect_semantics(*semantics_root, geometry, result.elements_);
    return result;
}

std::pair<double, double> BrowserModel::resolve(const Selector& selector) const {
    if (selector.x.has_value()) {
        if (!selector.y.has_value() || !std::isfinite(*selector.x) || !std::isfinite(*selector.y)) {
            throw std::invalid_argument("headless selector coordinates must be finite");
        }
        return {*selector.x, *selector.y};
    }
    std::vector<const BrowserElement*> matches;
    for (const BrowserElement& element : elements_) {
        if (selector.path.has_value() && element.path != *selector.path)
            continue;
        if (selector.key.has_value() && element.key != *selector.key)
            continue;
        if (selector.name.has_value() && element.name != *selector.name)
            continue;
        if (selector.role.has_value() && element.role != *selector.role)
            continue;
        matches.push_back(&element);
    }
    const bool key_only = selector.key.has_value() && !selector.path.has_value() &&
                          !selector.name.has_value() && !selector.role.has_value();
    if (matches.size() > 1U && key_only) {
        std::vector<const BrowserElement*> retained;
        for (const BrowserElement* match : matches) {
            if (!match->virtual_node)
                retained.push_back(match);
        }
        if (retained.size() == 1U)
            matches = std::move(retained);
    }
    if (matches.empty())
        throw std::runtime_error("headless selector matched no semantic element");
    if (matches.size() != 1U) {
        std::string detail;
        const std::size_t shown = std::min<std::size_t>(matches.size(), 4U);
        for (std::size_t index = 0U; index < shown; ++index) {
            if (!detail.empty())
                detail += ", ";
            detail +=
                matches[index]->role + " '" + matches[index]->name + "' at " + matches[index]->path;
        }
        throw std::runtime_error("headless selector is ambiguous (" +
                                 std::to_string(matches.size()) + " semantic elements: " + detail +
                                 ")");
    }
    const BrowserElement& match = *matches.front();
    if (!match.hit_bounds.has_value()) {
        throw std::runtime_error("headless selector matched semantic element '" + match.name +
                                 "' without currently clickable on-screen bounds");
    }
    return {match.hit_bounds->x + match.hit_bounds->width * 0.5,
            match.hit_bounds->y + match.hit_bounds->height * 0.5};
}

JsonValue BrowserModel::document() const {
    std::vector<JsonValue> values;
    values.reserve(elements_.size());
    for (const BrowserElement& element : elements_) {
        values.push_back(object({
            {"actions", element.actions},
            {"actionable",
             JsonValue(element.hit_bounds.has_value() && has_actions(element.actions))},
            {"hitBounds", encoded_bounds(element.hit_bounds)},
            {"key", element.key.empty() ? JsonValue{} : JsonValue(element.key)},
            {"name", JsonValue(element.name)},
            {"path", JsonValue(element.path)},
            {"role", JsonValue(element.role)},
            {"state", element.state},
            {"virtual", JsonValue(element.virtual_node)},
            {"visible", JsonValue(element.hit_bounds.has_value())},
        }));
    }
    return object({{"elements", array(std::move(values))}});
}

const std::vector<BrowserElement>& BrowserModel::elements() const noexcept {
    return elements_;
}

} // namespace strata::headless
