#include "ui/widget/registry.hpp"

#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/inspection.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/semantics.hpp"

#include <algorithm>
#include <atomic>
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
    refresh_trait_types();
}

void WidgetRegistry::register_participation(
    std::string type,
    WidgetParticipationHook participates
) {
    lifecycle(std::move(type)).participates = participates;
    refresh_trait_types();
}

WidgetLifecycle& WidgetRegistry::lifecycle(std::string type) {
    if (type.empty()) throw std::invalid_argument("widget lifecycle type must not be empty");
    auto [found, inserted] = lifecycles_.try_emplace(type);
    if (inserted) found->second.type = std::move(type);
    return found->second;
}

void WidgetRegistry::register_describe_phase(std::string type, WidgetDescribePhase phase) {
    lifecycle(std::move(type)).describe = std::move(phase);
    refresh_trait_types();
}

void WidgetRegistry::register_input_phase(std::string type, WidgetInputPhase phase) {
    lifecycle(std::move(type)).input = std::move(phase);
    refresh_trait_types();
}

void WidgetRegistry::register_semantics_phase(std::string type, WidgetSemanticsPhase phase) {
    lifecycle(std::move(type)).semantics = std::move(phase);
    refresh_trait_types();
}

void WidgetRegistry::register_inspection_phase(std::string type, WidgetInspectionPhase phase) {
    lifecycle(std::move(type)).inspection = std::move(phase);
    refresh_trait_types();
}

void WidgetRegistry::register_command_phase(std::string type, WidgetCommandPhase phase) {
    lifecycle(std::move(type)).command = std::move(phase);
    refresh_trait_types();
}

void WidgetRegistry::register_persistence_phase(
    std::string type,
    WidgetPersistencePhase phase
) {
    lifecycle(std::move(type)).persistence = std::move(phase);
    refresh_trait_types();
}

void WidgetRegistry::register_present_phase(std::string type, WidgetPresentPhase phase) {
    lifecycle(std::move(type)).present = std::move(phase);
    refresh_trait_types();
}

WidgetRegistry::WidgetRegistry(const WidgetRegistry& other) : lifecycles_(other.lifecycles_) {
    refresh_trait_types();
}

WidgetRegistry& WidgetRegistry::operator=(const WidgetRegistry& other) {
    if (this != &other) {
        lifecycles_ = other.lifecycles_;
        refresh_trait_types();
    }
    return *this;
}

namespace {

[[nodiscard]] std::uint64_t next_registry_revision() noexcept {
    static std::atomic<std::uint64_t> next{1U};
    return next.fetch_add(1U, std::memory_order_relaxed);
}

} // namespace

// Moving keeps the table's nodes, and so the trait lists' views of its keys.
WidgetRegistry::WidgetRegistry(WidgetRegistry&& other) noexcept
    : lifecycles_(std::move(other.lifecycles_)),
      command_declaration_types_(std::move(other.command_declaration_types_)),
      authored_presentation_types_(std::move(other.authored_presentation_types_)),
      detached_overlay_types_(std::move(other.detached_overlay_types_)),
      revision_(next_registry_revision()) {
    other.revision_ = next_registry_revision();
}

WidgetRegistry& WidgetRegistry::operator=(WidgetRegistry&& other) noexcept {
    if (this != &other) {
        lifecycles_ = std::move(other.lifecycles_);
        command_declaration_types_ = std::move(other.command_declaration_types_);
        authored_presentation_types_ = std::move(other.authored_presentation_types_);
        detached_overlay_types_ = std::move(other.detached_overlay_types_);
        revision_ = next_registry_revision();
        other.revision_ = next_registry_revision();
    }
    return *this;
}

void WidgetRegistry::refresh_trait_types() {
    revision_ = next_registry_revision();
    command_declaration_types_.clear();
    authored_presentation_types_.clear();
    detached_overlay_types_.clear();
    for (const auto& [type, lifecycle] : lifecycles_) {
        if (lifecycle.command.declaration)
            command_declaration_types_.push_back(type);
        if (!lifecycle.describe.authored_presentation_property.empty())
            authored_presentation_types_.push_back(type);
        if (lifecycle.present.overlay != nullptr && lifecycle.present.detached_overlay)
            detached_overlay_types_.push_back(type);
    }
    std::ranges::sort(command_declaration_types_);
    std::ranges::sort(authored_presentation_types_);
    std::ranges::sort(detached_overlay_types_);
}

std::span<const std::string_view> WidgetRegistry::command_declaration_types() const noexcept {
    return command_declaration_types_;
}
std::span<const std::string_view> WidgetRegistry::authored_presentation_types() const noexcept {
    return authored_presentation_types_;
}
std::span<const std::string_view> WidgetRegistry::detached_overlay_types() const noexcept {
    return detached_overlay_types_;
}

std::vector<std::string> WidgetRegistry::text_editable_types() const {
    std::vector<std::string> result;
    for (const auto& [type, lifecycle] : lifecycles_) {
        if (lifecycle.input.text_edit_mode != WidgetTextEditMode::none) {
            result.push_back(type);
        }
    }
    std::ranges::sort(result);
    return result;
}

} // namespace strata::ui
