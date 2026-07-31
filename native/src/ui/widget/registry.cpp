#include "ui/widget/registry.hpp"

#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/inspection.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/semantics.hpp"

#include <stdexcept>
#include <utility>

namespace strata::ui {
namespace {

[[nodiscard]] bool key_list(const runtime::Value& value) noexcept {
    if (value.list() == nullptr) return false;
    for (const runtime::Value& entry : value.list()->values) {
        if (entry.key() == nullptr && entry.string() == nullptr) return false;
    }
    return true;
}

[[nodiscard]] bool table_widths(const runtime::Value& value) noexcept {
    if (value.list() == nullptr) return false;
    for (const runtime::Value& entry : value.list()->values) {
        const runtime::Value* id = entry.field("id");
        const runtime::Value* width = entry.field("width");
        if (entry.object() == nullptr || id == nullptr ||
            (id->key() == nullptr && id->string() == nullptr) || width == nullptr ||
            width->number() == nullptr || *width->number() <= 0.0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool persisted_widget_value(
    const std::string_view field,
    const runtime::Value& value
) noexcept {
    if (field == "strata.scroll.offset") {
        const runtime::Value* x = value.field("x");
        const runtime::Value* y = value.field("y");
        return value.object() != nullptr && x != nullptr && x->number() != nullptr &&
            y != nullptr && y->number() != nullptr;
    }
    if (field == "strata.tree.expanded" || field == "strata.table.columnOrder") {
        return key_list(value);
    }
    if (field == "strata.table.columnWidths") return table_widths(value);
    if (field == "strata.gesture.splitRatio") return value.number() != nullptr;
    if (field == "$expanded") return value.boolean() != nullptr;
    return false;
}

} // namespace

WidgetRegistry::WidgetRegistry() {
    register_primitive_widget_descriptions(*this);
    register_control_widget_descriptions(*this);
    register_shell_widget_descriptions(*this);
    register_collection_widget_descriptions(*this);
    register_primitive_widget_inputs(*this);
    register_control_widget_inputs(*this);
    register_shell_widget_inputs(*this);
    register_collection_widget_inputs(*this);
    register_primitive_widget_inspection(*this);
    register_shell_widget_inspection(*this);
    register_collection_widget_inspection(*this);
    register_primitive_widget_semantics(*this);
    register_control_widget_semantics(*this);
    register_shell_widget_semantics(*this);
    register_collection_widget_semantics(*this);
    register_builtin_widget_presenters(*this);

    const WidgetPersistencePhase scroll{{"strata.scroll.offset"}, &persisted_widget_value};
    for (const std::string_view type : {
             "Scroll", "List", "VirtualList", "Table", "TreeView", "ItemGrid",
         }) {
        register_persistence_phase(std::string(type), scroll);
    }
    register_persistence_phase(
        "TreeView",
        WidgetPersistencePhase{
            {"strata.scroll.offset", "strata.tree.expanded"}, &persisted_widget_value
        }
    );
    register_persistence_phase(
        "Table",
        WidgetPersistencePhase{{
            "strata.scroll.offset", "strata.table.columnWidths", "strata.table.columnOrder",
        }, &persisted_widget_value}
    );
    register_persistence_phase(
        "SplitPane",
        WidgetPersistencePhase{{"strata.gesture.splitRatio"}, &persisted_widget_value}
    );
    register_persistence_phase(
        "Section", WidgetPersistencePhase{{"$expanded"}, &persisted_widget_value}
    );
}

void register_builtin_widget_presenters(WidgetRegistry& registry) {
    register_primitive_widget_presenters(registry);
    register_control_widget_presenters(registry);
    register_shell_widget_presenters(registry);
    register_collection_widget_presenters(registry);
}

const WidgetLifecycle* WidgetRegistry::find(const std::string_view type) const noexcept {
    const auto found = lifecycles_.find(type);
    return found != lifecycles_.end() ? &found->second : nullptr;
}

void WidgetRegistry::register_lifecycle(WidgetLifecycle lifecycle) {
    if (lifecycle.type.empty()) throw std::invalid_argument("widget lifecycle type must not be empty");
    const std::string type = lifecycle.type;
    if (!lifecycles_.emplace(type, std::move(lifecycle)).second) {
        throw std::invalid_argument("duplicate widget lifecycle for '" + type + "'");
    }
}

void WidgetRegistry::register_participation(
    std::string type,
    WidgetParticipationHook participates
) {
    lifecycle(std::move(type)).participates = participates;
}

WidgetLifecycle& WidgetRegistry::lifecycle(std::string type) {
    if (type.empty()) throw std::invalid_argument("widget lifecycle type must not be empty");
    auto [found, inserted] = lifecycles_.try_emplace(type);
    if (inserted) found->second.type = std::move(type);
    return found->second;
}

void WidgetRegistry::register_describe_phase(std::string type, WidgetDescribePhase phase) {
    lifecycle(std::move(type)).describe = std::move(phase);
}

void WidgetRegistry::register_input_phase(std::string type, WidgetInputPhase phase) {
    lifecycle(std::move(type)).input = std::move(phase);
}

void WidgetRegistry::register_semantics_phase(std::string type, WidgetSemanticsPhase phase) {
    lifecycle(std::move(type)).semantics = std::move(phase);
}

void WidgetRegistry::register_inspection_phase(std::string type, WidgetInspectionPhase phase) {
    lifecycle(std::move(type)).inspection = std::move(phase);
}

void WidgetRegistry::register_command_phase(std::string type, WidgetCommandPhase phase) {
    lifecycle(std::move(type)).command = std::move(phase);
}

void WidgetRegistry::register_persistence_phase(
    std::string type,
    WidgetPersistencePhase phase
) {
    lifecycle(std::move(type)).persistence = std::move(phase);
}

void WidgetRegistry::register_present_phase(std::string type, WidgetPresentPhase phase) {
    lifecycle(std::move(type)).present = std::move(phase);
}

std::vector<std::string> WidgetRegistry::text_editable_types() const {
    std::vector<std::string> result;
    for (const auto& [type, lifecycle] : lifecycles_) {
        if (lifecycle.input.text_edit_mode != WidgetTextEditMode::none) {
            result.push_back(type);
        }
    }
    return result;
}

} // namespace strata::ui
