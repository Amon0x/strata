#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "data/json.hpp"
#include "ui/layout.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {

class Surface;

/** Mutable inspection projection passed to one widget lifecycle inspection hook. */
class WidgetInspectionScope final {
public:
    WidgetInspectionScope(
        const Surface& surface,
        const RetainedNode& node,
        const LayoutRecord& layout
    );

    [[nodiscard]] const Surface& surface() const noexcept;
    [[nodiscard]] const RetainedNode& node() const noexcept;
    [[nodiscard]] const LayoutRecord& layout() const noexcept;
    [[nodiscard]] const runtime::Value* property(std::string_view name) const noexcept;
    [[nodiscard]] const runtime::Value* retained(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string> text(const runtime::Value* value) const;
    [[nodiscard]] std::optional<bool> boolean(const runtime::Value* value) const noexcept;
    [[nodiscard]] double number(std::string_view name, double fallback) const noexcept;
    [[nodiscard]] bool effective_boolean(
        std::string_view controlled,
        std::string_view retained_name,
        std::string_view initial,
        bool fallback
    ) const noexcept;

    void hit_bounds(Rect value) noexcept;
    void collection(data::JsonValue value);
    void derived_collection(data::JsonValue value);

    [[nodiscard]] const Rect& hit_bounds() const noexcept;
    [[nodiscard]] const data::JsonValue& collection() const noexcept;
    [[nodiscard]] const data::JsonValue& derived_collection() const noexcept;
    [[nodiscard]] bool has_derived_collection() const noexcept;

private:
    const Surface& surface_;
    const RetainedNode& node_;
    const LayoutRecord& layout_;
    Rect hit_bounds_;
    data::JsonValue collection_;
    data::JsonValue derived_collection_;
};

void register_primitive_widget_inspection(WidgetRegistry& registry);
void register_shell_widget_inspection(WidgetRegistry& registry);
void register_collection_widget_inspection(WidgetRegistry& registry);

/** Canonical widget-owned hit geometry shared by input routing and inspection output. */
[[nodiscard]] Rect widget_hit_bounds(
    const Surface& surface,
    const RetainedNode& node,
    const LayoutRecord& layout
);

} // namespace strata::ui
