#pragma once

#include <strata/strata.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/behavior/input.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/inspection.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/semantics.hpp"

namespace strata::abi_detail {

/** Declared retained surface of one widget extension, resolved once at surface creation. */
class ExtensionRetainedFields final {
public:
    void declare(std::string name, strata_widget_invalidation invalidation);
    [[nodiscard]] const strata_widget_invalidation* find(std::string_view name) const noexcept;

private:
    std::vector<std::pair<std::string, strata_widget_invalidation>> fields_;
};

} // namespace strata::abi_detail

struct strata_widget_input_context final {
    strata::ui::WidgetInputScope* scope = nullptr;
    const strata::abi_detail::ExtensionRetainedFields* fields = nullptr;
};

struct strata_widget_render_context final {
    strata::ui::WidgetRenderScope* scope = nullptr;
    const strata::abi_detail::ExtensionRetainedFields* fields = nullptr;
};

struct strata_widget_inspection_context final {
    strata::ui::WidgetInspectionScope* scope = nullptr;
};

struct strata_widget_semantics_context final {
    strata::ui::WidgetSemanticsScope* scope = nullptr;
    const strata::abi_detail::ExtensionRetainedFields* fields = nullptr;
    std::vector<std::string>* actions = nullptr;
};

struct strata_behavior_input_context final {
    strata::ui::BehaviorInputScope* scope = nullptr;
};

namespace strata::abi_detail {

struct ExtensionRegistries final {
    ui::WidgetRegistry widgets;
    ui::BehaviorRegistry behaviors;
};

[[nodiscard]] ExtensionRegistries extension_registries(
    const strata_surface_extension_bundle* bundle
);

} // namespace strata::abi_detail
