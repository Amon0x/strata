#include "ui/widget/input.hpp"
#include "ui/widget/shell_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::ui {
namespace {

[[nodiscard]] std::size_t menu_category_index(WidgetInputScope& scope) {
    const runtime::Value* open = scope.retained("$menuCategory");
    const std::string* category = open != nullptr ? open->string() : nullptr;
    if (category == nullptr || category->empty()) return 0U;
    std::size_t enabled_index = 0U;
    for (const WidgetSubtarget& target : scope.subtargets()) {
        if (target.detached || !target.enabled ||
            target.kind != WidgetSubtargetKind::control) {
            continue;
        }
        if (target.value.string() != nullptr && *target.value.string() == *category) {
            return enabled_index;
        }
        ++enabled_index;
    }
    return 0U;
}

bool breadcrumbs_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || target->kind != WidgetSubtargetKind::choice || !target->enabled) {
        return false;
    }
    scope.value_changed("onSelect", "selection-changed", target->value);
    return true;
}

bool command_surface_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || !target->enabled) return false;
    if (target->kind == WidgetSubtargetKind::command && !target->command_id.empty()) {
        const std::size_t category_index = menu_category_index(scope);
        const bool handled = scope.invoke_command(target->command_id);
        if (scope.node().description().type == "MenuBar") {
            scope.set_retained("$menuCategory", runtime::Value{}, DirtyReason::properties);
            scope.set_retained(
                "$shellIndex", runtime::Value(static_cast<double>(category_index)),
                DirtyReason::input
            );
        } else if (scope.node().description().type == "Toolbar") {
            scope.set_retained("$toolbarOverflow", runtime::Value(false), DirtyReason::properties);
            scope.set_retained("$toolbarOverflowOffset", runtime::Value(0.0), DirtyReason::input);
        }
        return handled;
    }
    if (scope.node().description().type == "Toolbar" && target->id == "$overflow") {
        const runtime::Value* retained = scope.retained("$toolbarOverflow");
        const bool open = retained != nullptr && retained->boolean() != nullptr &&
                          *retained->boolean();
        scope.set_retained("$toolbarOverflow", runtime::Value(!open), DirtyReason::properties);
        scope.set_retained("$toolbarOverflowOffset", runtime::Value(0.0), DirtyReason::input);
        scope.set_retained("$shellIndex", runtime::Value(0.0), DirtyReason::input);
        return true;
    }
    if (scope.node().description().type == "MenuBar" &&
        target->kind == WidgetSubtargetKind::control) {
        const std::string current = scope.retained("$menuCategory") != nullptr &&
            scope.retained("$menuCategory")->string() != nullptr
            ? *scope.retained("$menuCategory")->string()
            : std::string{};
        const std::string next = target->value.string() != nullptr
            ? *target->value.string()
            : std::string{};
        scope.set_retained(
            "$menuCategory",
            current == next ? runtime::Value{} : runtime::Value(next),
            DirtyReason::properties
        );
        scope.set_retained(
            "$shellIndex",
            runtime::Value(current == next ? static_cast<double>(target->index) : 0.0),
            DirtyReason::input
        );
        return true;
    }
    return false;
}

void retain_banner_subtarget(WidgetInputScope& scope, const WidgetSubtarget& target) {
    scope.set_retained(
        std::string(banner_active_subtarget_state), runtime::Value(target.id), DirtyReason::input
    );
}

bool activate_banner_subtarget(
    WidgetInputScope& scope,
    const WidgetSubtarget& target
) {
    const bool action = target.id == banner_action_subtarget &&
                        target.kind == WidgetSubtargetKind::action;
    const bool dismiss = target.id == banner_dismiss_subtarget &&
                         target.kind == WidgetSubtargetKind::dismiss;
    if (!target.enabled || (!action && !dismiss)) return false;
    retain_banner_subtarget(scope, target);
    if (action) {
        scope.activated("action");
        return true;
    }
    if (dismiss) {
        scope.set_retained("$dismissed", runtime::Value(true), DirtyReason::layout);
        scope.emit_event("dismissed");
        return true;
    }
    return false;
}

[[nodiscard]] std::optional<WidgetSubtarget> banner_subtarget(
    WidgetInputScope& scope,
    const std::string_view id
) {
    const std::vector<WidgetSubtarget> targets = scope.subtargets();
    const auto found = std::ranges::find(targets, id, &WidgetSubtarget::id);
    return found != targets.end() && found->enabled
        ? std::optional<WidgetSubtarget>(*found)
        : std::nullopt;
}

bool banner_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || !target->enabled) return false;
    return activate_banner_subtarget(scope, *target);
}

bool banner_key(WidgetInputScope& scope) {
    if (scope.key() == "escape") {
        const std::optional<WidgetSubtarget> dismiss = banner_subtarget(
            scope, banner_dismiss_subtarget
        );
        return dismiss.has_value() && activate_banner_subtarget(scope, *dismiss);
    }

    if (scope.key() == "left" || scope.key() == "right" ||
        scope.key() == "up" || scope.key() == "down" ||
        scope.key() == "home" || scope.key() == "end") {
        const std::optional<WidgetSubtarget> action = banner_subtarget(
            scope, banner_action_subtarget
        );
        const std::optional<WidgetSubtarget> dismiss = banner_subtarget(
            scope, banner_dismiss_subtarget
        );
        if (!action.has_value() && !dismiss.has_value()) return false;
        const bool leading = scope.key() == "left" || scope.key() == "up" ||
                             scope.key() == "home";
        const WidgetSubtarget& target = leading && action.has_value()
            ? *action
            : dismiss.has_value() ? *dismiss : *action;
        retain_banner_subtarget(scope, target);
        return true;
    }

    if (scope.key() != "enter" && scope.key() != "space") return false;
    std::optional<WidgetSubtarget> target;
    const runtime::Value* retained = scope.retained(banner_active_subtarget_state);
    const std::string* retained_id = retained != nullptr ? retained->string() : nullptr;
    if (retained_id != nullptr) target = banner_subtarget(scope, *retained_id);
    if (!target.has_value()) target = banner_subtarget(scope, banner_action_subtarget);
    if (!target.has_value()) target = banner_subtarget(scope, banner_dismiss_subtarget);
    if (!target.has_value()) return false;
    return activate_banner_subtarget(scope, *target);
}

bool modal_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || target->kind != WidgetSubtargetKind::scrim) return false;
    scope.activated("onDismiss");
    return true;
}

bool form_click(WidgetInputScope& scope) {
    if (scope.pointer() != nullptr && scope.pointer_target() != &scope.node()) return false;
    scope.activated({});
    return true;
}

bool form_key(WidgetInputScope& scope) {
    if (scope.key() != "enter") return false;
    const RetainedNode* target = scope.dispatch() != nullptr
        ? scope.dispatch()->target()
        : nullptr;
    if (target != nullptr && target != &scope.node() &&
        target->description().type != "TextBox" &&
        target->description().type != "NumberField") {
        return false;
    }
    scope.activated({});
    return true;
}

void close_palette(WidgetInputScope& scope) {
    scope.set_retained("strata.palette.open", runtime::Value(false), DirtyReason::properties);
    scope.set_retained("$paletteActive", runtime::Value(0.0), DirtyReason::input);
    static_cast<void>(scope.clear_editor_text());
    scope.synchronize_modal_focus();
}

[[nodiscard]] std::optional<PaletteProjection> palette_projection(WidgetInputScope& scope) {
    const LayoutResult* layout = scope.layout_result();
    if (layout == nullptr) return std::nullopt;
    const std::string* query = scope.editor_text();
    return project_command_palette(
        scope.node(),
        *layout,
        scope.command_index(),
        query != nullptr ? std::string_view(*query) : std::string_view{}
    );
}

bool activate_palette_target(WidgetInputScope& scope, const WidgetSubtarget& target) {
    if (!target.enabled) return false;
    bool handled = false;
    if (target.kind == WidgetSubtargetKind::command && !target.command_id.empty()) {
        handled = scope.invoke_command(target.command_id);
    } else if (target.kind == WidgetSubtargetKind::choice) {
        scope.value_changed("onSelect", "selection-changed", target.value);
        handled = true;
    }
    if (handled) close_palette(scope);
    return handled;
}

bool command_palette_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    const WidgetSubtarget* target = scope.subtarget();
    if (pointer == nullptr || target == nullptr) return false;
    if (pointer->type == PointerEventType::move &&
        (target->kind == WidgetSubtargetKind::command ||
         target->kind == WidgetSubtargetKind::choice)) {
        scope.set_retained(
            "$paletteActive",
            runtime::Value(static_cast<double>(target->index)),
            DirtyReason::input
        );
        return true;
    }
    return false;
}

bool command_palette_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr) return false;
    if (target->kind == WidgetSubtargetKind::scrim) {
        close_palette(scope);
        return true;
    }
    if (target->id == "$editor" || target->id == "$panel") return true;
    return activate_palette_target(scope, *target);
}

bool command_palette_text(WidgetInputScope& scope) {
    scope.set_retained("$paletteActive", runtime::Value(0.0), DirtyReason::input);
    return false;
}

bool command_palette_key(WidgetInputScope& scope) {
    if (scope.key() == "escape") {
        close_palette(scope);
        return true;
    }
    if (scope.key() == "backspace" || scope.key() == "delete" ||
        scope.modifiers().control || scope.modifiers().super_key) {
        scope.set_retained("$paletteActive", runtime::Value(0.0), DirtyReason::input);
        return false;
    }
    std::optional<PaletteProjection> projection = palette_projection(scope);
    if (!projection.has_value()) return false;
    if (scope.key() == "enter") {
        const PaletteEntryModel* entry = projection->active();
        if (entry == nullptr) return true;
        const auto targets = scope.subtargets();
        const auto target = std::ranges::find_if(targets, [entry](const WidgetSubtarget& value) {
            return value.id == "$palette/" + entry->id;
        });
        if (target != targets.end()) return activate_palette_target(scope, *target);
        if (!entry->command_id.empty()) {
            const bool handled = scope.invoke_command(entry->command_id);
            if (handled) close_palette(scope);
            return handled;
        }
        scope.value_changed("onSelect", "selection-changed", runtime::Value(entry->id));
        close_palette(scope);
        return true;
    }
    if (projection->matches.empty()) return false;
    std::size_t active = projection->active_index;
    if (scope.key() == "up") {
        active = active == 0U ? projection->matches.size() - 1U : active - 1U;
    } else if (scope.key() == "down") {
        active = (active + 1U) % projection->matches.size();
    } else if (scope.key() == "pageup") {
        active = active > projection->max_visible_rows
            ? active - projection->max_visible_rows
            : 0U;
    } else if (scope.key() == "pagedown") {
        active = std::min(
            projection->matches.size() - 1U,
            active + projection->max_visible_rows
        );
    } else {
        return false;
    }
    scope.set_retained(
        "$paletteActive", runtime::Value(static_cast<double>(active)), DirtyReason::input
    );
    return true;
}

bool toast_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || !target->notification_id.has_value()) return false;
    NotificationService& notifications = scope.notifications();
    const Notification* current = notifications.find(*target->notification_id);
    if (current == nullptr) return false;
    if (target->kind == WidgetSubtargetKind::dismiss) {
        return notifications.dismiss(current->id);
    }
    if (target->kind == WidgetSubtargetKind::action && current->request.action != nullptr) {
        const std::shared_ptr<const runtime::ActionValue> action = current->request.action;
        const std::uint64_t id = current->id;
        scope.dispatch_action(action, "notification-action", runtime::Value(static_cast<double>(id)));
        static_cast<void>(notifications.dismiss(id));
        return true;
    }
    return false;
}

[[nodiscard]] const runtime::ValueList* chip_values(const WidgetInputScope& scope) noexcept {
    const runtime::Value* source = scope.property("values");
    if (source == nullptr || source->list() == nullptr) source = scope.retained("$values");
    if (source == nullptr || source->list() == nullptr) source = scope.property("defaultValues");
    return source != nullptr ? source->list() : nullptr;
}

[[nodiscard]] std::vector<std::string> chip_strings(const WidgetInputScope& scope) {
    std::vector<std::string> result;
    const runtime::ValueList* source = chip_values(scope);
    if (source == nullptr) return result;
    result.reserve(source->values.size());
    for (const runtime::Value& value : source->values) {
        const std::string* text = value.string();
        if (text != nullptr) result.push_back(*text);
    }
    return result;
}

[[nodiscard]] runtime::Value chip_value(const std::vector<std::string>& values) {
    std::vector<runtime::Value> result;
    result.reserve(values.size());
    for (const std::string& value : values) result.emplace_back(value);
    return runtime::Value(std::move(result));
}

[[nodiscard]] bool chip_controlled(const WidgetInputScope& scope) noexcept {
    const runtime::Value* source = scope.property("values");
    return source != nullptr && source->list() != nullptr;
}

[[nodiscard]] std::size_t chip_maximum(const WidgetInputScope& scope) noexcept {
    const double value = scope.number("maxChips", 32.0);
    if (!std::isfinite(value) || value < 1.0) return 1U;
    return static_cast<std::size_t>(std::min(
        value,
        static_cast<double>(std::numeric_limits<std::size_t>::max())
    ));
}

[[nodiscard]] std::optional<std::size_t> active_chip(
    const WidgetInputScope& scope,
    const std::size_t count
) noexcept {
    const runtime::Value* active = scope.retained("$activeToken");
    if (active == nullptr || active->number() == nullptr || !std::isfinite(*active->number()) ||
        *active->number() < 0.0 || *active->number() >= static_cast<double>(count)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*active->number());
}

void set_active_chip(WidgetInputScope& scope, const std::optional<std::size_t> index) {
    scope.set_retained(
        "$activeToken",
        index.has_value() ? runtime::Value(static_cast<double>(*index)) : runtime::Value{},
        DirtyReason::input
    );
}

void publish_chips(WidgetInputScope& scope, const std::vector<std::string>& values) {
    runtime::Value published = chip_value(values);
    if (!chip_controlled(scope)) {
        scope.set_retained("$values", published, DirtyReason::properties);
    }
    scope.value_changed("onChange", "values-changed", std::move(published));
}

[[nodiscard]] std::string trim_chip(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1U);
    }
    return std::string(value);
}

void commit_chip(
    WidgetInputScope& scope,
    std::vector<std::string>& values,
    const std::string_view draft
) {
    const std::string value = trim_chip(draft);
    if (value.empty() || values.size() >= chip_maximum(scope) ||
        std::ranges::contains(values, value)) {
        return;
    }
    values.push_back(value);
    publish_chips(scope, values);
}

bool chip_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target != nullptr && target->kind == WidgetSubtargetKind::token) {
        set_active_chip(scope, target->index);
        return true;
    }
    set_active_chip(scope, std::nullopt);
    return target != nullptr && target->id == "$editor";
}

bool chip_text(WidgetInputScope& scope) {
    set_active_chip(scope, std::nullopt);
    const std::string_view input = scope.input_text();
    if (input.find_first_of(",;") == std::string_view::npos) return false;

    std::vector<std::string> values = chip_strings(scope);
    std::size_t segment_start = 0U;
    while (segment_start < input.size()) {
        const std::size_t index = input.find_first_of(",;", segment_start);
        if (index == std::string_view::npos) break;
        if (index > segment_start) {
            static_cast<void>(scope.insert_editor_text(input.substr(segment_start, index - segment_start)));
        }
        if (const std::string* draft = scope.editor_text(); draft != nullptr) {
            commit_chip(scope, values, *draft);
        }
        static_cast<void>(scope.clear_editor_text());
        segment_start = index + 1U;
    }
    if (segment_start < input.size()) {
        static_cast<void>(scope.insert_editor_text(input.substr(segment_start)));
    }
    return true;
}

bool chip_key(WidgetInputScope& scope) {
    std::vector<std::string> values = chip_strings(scope);
    const std::optional<std::size_t> active = active_chip(scope, values.size());
    const std::string* draft_value = scope.editor_text();
    const std::string_view draft = draft_value != nullptr ? *draft_value : std::string_view{};
    const bool command = scope.modifiers().control || scope.modifiers().super_key;

    if (command) {
        set_active_chip(scope, std::nullopt);
        return false;
    }
    if (scope.key() == "enter") {
        commit_chip(scope, values, draft);
        static_cast<void>(scope.clear_editor_text());
        set_active_chip(scope, std::nullopt);
        return true;
    }
    if (scope.key() == "backspace") {
        if (!draft.empty()) {
            set_active_chip(scope, std::nullopt);
            return false;
        }
        if (values.empty()) return true;
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(active.value_or(values.size() - 1U)));
        publish_chips(scope, values);
        set_active_chip(scope, std::nullopt);
        return true;
    }
    if (scope.key() == "delete") {
        if (!active.has_value()) return false;
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(*active));
        publish_chips(scope, values);
        set_active_chip(scope, std::nullopt);
        return true;
    }
    if (scope.key() == "left") {
        if (!draft.empty() || values.empty()) {
            set_active_chip(scope, std::nullopt);
            return false;
        }
        const std::size_t next = active.has_value() && *active > 0U
            ? *active - 1U
            : active.has_value() ? 0U : values.size() - 1U;
        set_active_chip(scope, next);
        return true;
    }
    if (scope.key() == "right" && active.has_value()) {
        set_active_chip(
            scope,
            *active + 1U < values.size()
                ? std::optional<std::size_t>(*active + 1U)
                : std::nullopt
        );
        return true;
    }
    return false;
}

bool indexed_key(WidgetInputScope& scope) {
    std::vector<WidgetSubtarget> targets = scope.subtargets();
    const bool toolbar = scope.node().description().type == "Toolbar";
    const bool menu_bar = scope.node().description().type == "MenuBar";
    const auto overflow_target = std::ranges::find(
        targets, std::string_view("$overflow"), &WidgetSubtarget::id
    );
    const std::size_t toolbar_hidden_count = overflow_target != targets.end() &&
        overflow_target->value.number() != nullptr && *overflow_target->value.number() >= 0.0
        ? static_cast<std::size_t>(*overflow_target->value.number())
        : 0U;
    const bool toolbar_open = toolbar &&
        scope.retained("$toolbarOverflow") != nullptr &&
        scope.retained("$toolbarOverflow")->boolean() != nullptr &&
        *scope.retained("$toolbarOverflow")->boolean();
    const runtime::Value* menu_category = menu_bar ? scope.retained("$menuCategory") : nullptr;
    const bool menu_open = menu_category != nullptr && menu_category->string() != nullptr &&
        !menu_category->string()->empty();
    const bool popup_open = toolbar_open || menu_open;
    std::vector<WidgetSubtarget> categories;
    if (menu_bar) {
        for (const WidgetSubtarget& target : targets) {
            if (!target.detached && target.enabled &&
                target.kind == WidgetSubtargetKind::control) {
                categories.push_back(target);
            }
        }
    }
    const std::size_t category_index = menu_category_index(scope);
    if (menu_open && (scope.key() == "left" || scope.key() == "right")) {
        if (categories.empty()) return false;
        const std::size_t next = scope.key() == "left"
            ? (category_index + categories.size() - 1U) % categories.size()
            : (category_index + 1U) % categories.size();
        scope.set_retained("$menuCategory", categories[next].value, DirtyReason::properties);
        scope.set_retained("$shellIndex", runtime::Value(0.0), DirtyReason::input);
        return true;
    }
    std::erase_if(targets, [popup_open](const WidgetSubtarget& target) {
        return !target.enabled || (popup_open ? !target.detached : target.detached);
    });
    if (targets.empty()) return false;
    std::size_t index = 0U;
    if (const runtime::Value* retained = scope.retained("$shellIndex");
        retained != nullptr && retained->number() != nullptr && *retained->number() >= 0.0) {
        index = std::min(static_cast<std::size_t>(*retained->number()), targets.size() - 1U);
    }
    if (toolbar && ((!toolbar_open && (scope.key() == "up" || scope.key() == "down")) ||
                    (toolbar_open && (scope.key() == "left" || scope.key() == "right")))) {
        return false;
    }
    if (menu_bar && !menu_open && (scope.key() == "up" || scope.key() == "down")) {
        scope.set_retained("$menuCategory", targets[index].value, DirtyReason::properties);
        scope.set_retained("$shellIndex", runtime::Value(0.0), DirtyReason::input);
        return true;
    }
    if (scope.key() == "left" || scope.key() == "up") {
        if (toolbar_open && index == 0U) {
            const runtime::Value* retained = scope.retained("$toolbarOverflowOffset");
            const std::size_t offset = retained != nullptr && retained->number() != nullptr &&
                *retained->number() >= 0.0
                ? static_cast<std::size_t>(*retained->number())
                : 0U;
            if (offset > 0U) {
                scope.set_retained(
                    "$toolbarOverflowOffset", runtime::Value(static_cast<double>(offset - 1U)),
                    DirtyReason::input
                );
                return true;
            }
        }
        index = (index + targets.size() - 1U) % targets.size();
    } else if (scope.key() == "right" || scope.key() == "down") {
        if (toolbar_open && index + 1U == targets.size()) {
            const runtime::Value* retained = scope.retained("$toolbarOverflowOffset");
            const std::size_t offset = retained != nullptr && retained->number() != nullptr &&
                *retained->number() >= 0.0
                ? static_cast<std::size_t>(*retained->number())
                : 0U;
            const std::size_t maximum_offset = toolbar_hidden_count > targets.size()
                ? toolbar_hidden_count - targets.size()
                : 0U;
            if (offset < maximum_offset) {
                scope.set_retained(
                    "$toolbarOverflowOffset", runtime::Value(static_cast<double>(offset + 1U)),
                    DirtyReason::input
                );
                return true;
            }
        }
        index = (index + 1U) % targets.size();
    } else if (scope.key() == "home") {
        index = 0U;
    } else if (scope.key() == "end") {
        index = targets.size() - 1U;
    } else if (scope.key() == "escape") {
        if (menu_bar) {
            scope.set_retained("$menuCategory", runtime::Value{}, DirtyReason::properties);
            scope.set_retained(
                "$shellIndex", runtime::Value(static_cast<double>(category_index)),
                DirtyReason::input
            );
            return true;
        }
        if (toolbar) {
            scope.set_retained("$toolbarOverflow", runtime::Value(false), DirtyReason::properties);
            scope.set_retained("$toolbarOverflowOffset", runtime::Value(0.0), DirtyReason::input);
            return true;
        }
    } else if (scope.key() == "enter" || scope.key() == "space") {
        const WidgetSubtarget& target = targets[index];
        if (target.kind == WidgetSubtargetKind::command) {
            const bool handled = scope.invoke_command(target.command_id);
            if (toolbar) {
                scope.set_retained(
                    "$toolbarOverflow", runtime::Value(false), DirtyReason::properties
                );
                scope.set_retained(
                    "$toolbarOverflowOffset", runtime::Value(0.0), DirtyReason::input
                );
            } else if (menu_bar) {
                scope.set_retained("$menuCategory", runtime::Value{}, DirtyReason::properties);
                scope.set_retained(
                    "$shellIndex", runtime::Value(static_cast<double>(category_index)),
                    DirtyReason::input
                );
            }
            return handled;
        }
        if (scope.node().description().type == "Breadcrumbs") {
            scope.value_changed("onSelect", "selection-changed", target.value);
            return true;
        }
        if (menu_bar) {
            scope.set_retained("$menuCategory", target.value, DirtyReason::properties);
            scope.set_retained("$shellIndex", runtime::Value(0.0), DirtyReason::input);
            return true;
        }
        if (toolbar && target.id == "$overflow") {
            const runtime::Value* retained = scope.retained("$toolbarOverflow");
            const bool open = retained != nullptr && retained->boolean() != nullptr &&
                              *retained->boolean();
            scope.set_retained(
                "$toolbarOverflow", runtime::Value(!open), DirtyReason::properties
            );
            scope.set_retained("$toolbarOverflowOffset", runtime::Value(0.0), DirtyReason::input);
            scope.set_retained("$shellIndex", runtime::Value(0.0), DirtyReason::input);
            return true;
        }
        return false;
    } else {
        return false;
    }
    scope.set_retained("$shellIndex", runtime::Value(static_cast<double>(index)), DirtyReason::input);
    return true;
}

} // namespace

void register_shell_widget_inputs(WidgetRegistry& registry) {
    WidgetInputPhase modal;
    modal.click = &modal_click;
    modal.action_property = "onDismiss";
    modal.fallback_action = "modal-dismiss";
    modal.action_capability_requires_binding = true;
    modal.focusable = true;
    modal.focusable_when = "open";
    registry.register_input_phase("Modal", std::move(modal));
    WidgetInputPhase palette;
    palette.pointer = &command_palette_pointer;
    palette.click = &command_palette_click;
    palette.text = &command_palette_text;
    palette.editor_key = &command_palette_key;
    palette.action_property = "onSelect";
    palette.focusable = true;
    palette.tabbable = false;
    palette.popup_controlled = "open";
    palette.popup_retained = "strata.palette.open";
    palette.popup_initial = "defaultOpen";
    palette.text_edit_mode = WidgetTextEditMode::single_line;
    palette.editor_emits_change = false;
    registry.register_input_phase("CommandPalette", std::move(palette));
    WidgetInputPhase toast;
    toast.click = &toast_click;
    toast.pointer_focus = WidgetPointerFocusPolicy::preserve;
    toast.tabbable = false;
    registry.register_input_phase("ToastRegion", std::move(toast));
    WidgetInputPhase chip;
    chip.click = &chip_click;
    chip.text = &chip_text;
    chip.editor_key = &chip_key;
    chip.action_property = "onChange";
    chip.focusable = true;
    chip.text_edit_mode = WidgetTextEditMode::single_line;
    chip.editor_emits_change = false;
    registry.register_input_phase("ChipInput", std::move(chip));
    WidgetInputPhase breadcrumbs;
    breadcrumbs.click = &breadcrumbs_click;
    breadcrumbs.key = &indexed_key;
    breadcrumbs.action_property = "onSelect";
    breadcrumbs.focusable = true;
    registry.register_input_phase("Breadcrumbs", std::move(breadcrumbs));
    WidgetInputPhase toolbar;
    toolbar.click = &command_surface_click;
    toolbar.key = &indexed_key;
    toolbar.focusable = true;
    toolbar.popup_retained = "$toolbarOverflow";
    registry.register_input_phase("Toolbar", toolbar);
    toolbar.popup_retained.clear();
    registry.register_input_phase("MenuBar", std::move(toolbar));
    WidgetInputPhase banner;
    banner.click = &banner_click;
    banner.key = &banner_key;
    banner.action_property = "action";
    banner.action_capability_requires_binding = true;
    banner.focusable = true;
    registry.register_input_phase("Banner", std::move(banner));
    registry.register_participation("Banner", [](const RetainedNode& node) noexcept {
        return project_banner(node).active;
    });
    WidgetInputPhase form;
    form.click = &form_click;
    form.key = &form_key;
    form.focusable = true;
    form.tabbable = false;
    registry.register_input_phase("Form", std::move(form));
}

} // namespace strata::ui
