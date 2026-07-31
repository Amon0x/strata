#include "ui/widget/semantics.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/notification.hpp"
#include "ui/widget/shell_model.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] std::optional<std::string> text(
    WidgetSemanticsScope& scope,
    const std::string_view property
) {
    return scope.text(scope.property(property));
}

void label_or_key(WidgetSemanticsScope& scope) {
    if (const auto label = text(scope, "label"); label.has_value()) scope.name(*label);
    else if (scope.node().description().key.has_value()) {
        scope.default_name(*scope.node().description().key);
    }
}

void referenced_commands(
    WidgetSemanticsScope& scope,
    const std::string_view role
) {
    if (scope.command_index() == nullptr) return;
    CommandReferenceProjection projection = scope.command_index()->reference_projection(scope.node());
    std::vector<const CommandSnapshot*> commands = std::move(projection.commands);
    for (std::size_t index = 0U; index < commands.size(); ++index) {
        scope.virtual_before(scope.virtual_command(*commands[index], index, role));
    }
}

void menu_bar(WidgetSemanticsScope& scope) {
    scope.name("Menu bar");
    referenced_commands(scope, "menu_item");
    const runtime::Value* category = scope.retained("$menuCategory");
    const bool expanded = category != nullptr && category->string() != nullptr &&
        !category->string()->empty();
    scope.expanded(expanded);
    scope.default_value_text(expanded ? "true" : "false");
    scope.actions({"expand", "focus"});
}

void toolbar(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    referenced_commands(scope, "button");
    scope.actions({"focus"});
}

void command_palette(WidgetSemanticsScope& scope) {
    if (const auto label = text(scope, "label"); label.has_value()) scope.name(*label);
    else scope.name("Command palette");
    const bool open = scope.effective_boolean(
        "open", "strata.palette.open", "defaultOpen", false
    );
    scope.expanded(open);
    scope.actions({open ? "collapse" : "expand", "focus"});
    scope.default_value_text(open ? "true" : "false");
    referenced_commands(scope, "menu_item");
}

void focus_group(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.actions({"focus"});
}

void chip_input(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.actions({"focus"});
    const runtime::Value* source = scope.property("values");
    if (source == nullptr || source->list() == nullptr) source = scope.retained("$values");
    if (source == nullptr || source->list() == nullptr) source = scope.property("defaultValues");
    const runtime::ValueList* values = source != nullptr ? source->list() : nullptr;
    const runtime::Value* active_value = scope.retained("$activeToken");
    const std::optional<std::size_t> active = active_value != nullptr &&
        active_value->number() != nullptr && *active_value->number() >= 0.0
        ? std::optional<std::size_t>(static_cast<std::size_t>(*active_value->number()))
        : std::nullopt;
    std::string summary;
    if (values != nullptr) {
        for (std::size_t index = 0U; index < values->values.size(); ++index) {
            const std::optional<std::string> label = scope.text(&values->values[index]);
            if (!label.has_value()) continue;
            if (!summary.empty()) summary += ", ";
            summary += *label;
            scope.virtual_before(scope.virtual_item(
                index,
                5'000'000U,
                "list_item",
                *label,
                {"focus"},
                std::nullopt,
                std::optional<bool>(active == index),
                false,
                index
            ));
        }
    }
    if (const std::string* draft = scope.edited_text(); draft != nullptr && !draft->empty()) {
        if (!summary.empty()) summary += ", ";
        summary += *draft;
    }
    scope.default_value_text(std::move(summary));
}

void banner(WidgetSemanticsScope& scope) {
    const BannerProjection banner = project_banner(scope.node());
    if (!banner.active) {
        scope.actions({});
        return;
    }
    if (const auto message = text(scope, "message"); message.has_value()) scope.name(*message);
    else label_or_key(scope);
    scope.live_region("assertive");
    std::vector<std::string> actions;
    if (banner.has_action) actions.emplace_back("activate");
    if (banner.dismissible) actions.emplace_back("dismiss");
    // Focus is appended later by the shared input-capability projection iff this node is focusable.
    scope.actions(std::move(actions));
}

void toast_region(WidgetSemanticsScope& scope) {
    scope.name("Notifications");
    scope.live_region("polite");
    const NotificationService* notifications = scope.notifications();
    const runtime::Value* maximum = scope.property("maxVisible");
    const std::size_t max_visible = maximum != nullptr && maximum->number() != nullptr &&
        std::isfinite(*maximum->number())
        ? static_cast<std::size_t>(std::clamp(*maximum->number(), 1.0, 4'096.0))
        : 3U;
    const NotificationSnapshot snapshot = notifications != nullptr
        ? notifications->snapshot(max_visible)
        : NotificationSnapshot{};
    for (const Notification& notification : snapshot.visible) {
        std::vector<std::string> actions{"dismiss"};
        if (notification.request.action != nullptr) actions.emplace_back("activate");
        const bool urgent = notification.request.severity == NotificationSeverity::error;
        scope.virtual_before(scope.virtual_item(
            static_cast<std::size_t>(notification.id),
            0U,
            urgent ? "alert" : "status",
            notification.request.message,
            std::move(actions),
            std::nullopt,
            std::nullopt,
            false,
            std::nullopt,
            std::nullopt,
            {},
            std::string(scope.node().structural_path()),
            std::nullopt,
            urgent ? "assertive" : "polite",
            notification.id
        ));
    }
    const std::string overflow = snapshot.overflow_count != 0U
        ? std::to_string(snapshot.overflow_count) + " more notifications"
        : std::string{};
    scope.value_range(object({
        {"current", JsonValue(static_cast<double>(notifications != nullptr
            ? notifications->size() : 0U))},
        {"maximum", JsonValue{}},
        {"minimum", JsonValue(0.0)},
        {"text", overflow.empty() ? JsonValue{} : JsonValue(overflow)},
    }));
}

void modal(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    if (!scope.boolean(scope.property("open")).value_or(true)) {
        scope.actions({});
        return;
    }
    if (scope.has_action("onDismiss")) scope.actions({"dismiss", "focus"});
}

void field(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.dirty(scope.boolean(scope.retained("strata.form.dirty")));
    scope.touched(scope.boolean(scope.retained("strata.form.touched")));
    scope.required(scope.boolean(scope.property("required")));
    const runtime::Value* controlled = scope.property("error");
    const runtime::Value* retained = scope.retained("strata.form.error");
    scope.invalid(
        (controlled != nullptr && controlled->string() != nullptr &&
         !controlled->string()->empty()) ||
        (retained != nullptr && retained->string() != nullptr && !retained->string()->empty())
    );
}

void form(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.actions({"activate"});
}

void split_pane(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    const runtime::Value* ratio = scope.property("ratio");
    if (ratio == nullptr || ratio->number() == nullptr) ratio = scope.property("defaultRatio");
    const runtime::Value* minimum = scope.property("minRatio");
    const runtime::Value* maximum = scope.property("maxRatio");
    scope.value_range(object({
        {"current", ratio != nullptr && ratio->number() != nullptr
                        ? JsonValue(*ratio->number()) : JsonValue(0.5)},
        {"maximum", maximum != nullptr && maximum->number() != nullptr
                        ? JsonValue(*maximum->number()) : JsonValue(0.9)},
        {"minimum", minimum != nullptr && minimum->number() != nullptr
                        ? JsonValue(*minimum->number()) : JsonValue(0.1)},
        {"text", JsonValue{}},
    }));
}

void tooltip(WidgetSemanticsScope& scope) {
    if (const auto value = text(scope, "text"); value.has_value()) scope.name(*value);
    else label_or_key(scope);
}

void add(
    WidgetRegistry& registry,
    std::string type,
    std::string role,
    const WidgetSemanticsHook derive = nullptr,
    const bool hidden = false
) {
    registry.register_semantics_phase(
        std::move(type),
        WidgetSemanticsPhase{std::move(role), {}, derive, hidden, false}
    );
}

} // namespace

void register_shell_widget_semantics(WidgetRegistry& registry) {
    add(registry, "MenuBar", "menu", &menu_bar);
    add(registry, "Toolbar", "group", &toolbar);
    add(registry, "CommandPalette", "combo_box", &command_palette);
    add(registry, "ChipInput", "group", &chip_input);
    add(registry, "Breadcrumbs", "group", &focus_group);
    add(registry, "Banner", "alert", &banner);
    add(registry, "ToastRegion", "status", &toast_region);
    add(registry, "Modal", "dialog", &modal);
    add(registry, "Form", "form", &form);
    add(registry, "Field", "field", &field);
    add(registry, "SplitPane", "group", &split_pane);
    add(registry, "Tooltip", "group", &tooltip);
    add(registry, "Command", "group", nullptr, true);
}

} // namespace strata::ui
