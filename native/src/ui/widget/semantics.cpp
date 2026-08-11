#include "ui/widget/semantics.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <utility>

#include "ui/input.hpp"
#include "ui/motion.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue nullable_boolean(const std::optional<bool> value) {
    return value.has_value() ? JsonValue(*value) : JsonValue{};
}

[[nodiscard]] JsonValue nullable_text(const std::optional<std::string>& value) {
    return value.has_value() ? JsonValue(*value) : JsonValue{};
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

[[nodiscard]] std::vector<std::string> actions_for(const std::string_view role) {
    if (role == "button" || role == "link")
        return {"activate", "focus"};
    if (role == "switch" || role == "checkbox")
        return {"focus", "toggle"};
    if (role == "text_field" || role == "spin_button")
        return {"activate", "focus"};
    if (role == "slider")
        return {"decrement", "focus", "increment"};
    if (role == "tab" || role == "tab_list")
        return {"activate", "focus"};
    if (role == "dialog")
        return {"focus"};
    if (role == "combo_box")
        return {"expand", "focus"};
    if (role == "tree" || role == "tree_item")
        return {"focus"};
    if (role == "radio_group")
        return {"activate", "focus"};
    if (role == "table" || role == "grid" || role == "grid_cell")
        return {"focus"};
    return role == "text" ? std::vector<std::string>{"focus"} : std::vector<std::string>{};
}

[[nodiscard]] JsonValue semantic_state(const std::optional<bool> selected = std::nullopt) {
    return object({
        {"checked", JsonValue{}},
        {"dirty", JsonValue{}},
        {"disabled", JsonValue(false)},
        {"expanded", JsonValue{}},
        {"invalid", JsonValue{}},
        {"readOnly", JsonValue{}},
        {"required", JsonValue{}},
        {"selected", nullable_boolean(selected)},
        {"touched", JsonValue{}},
        {"value", JsonValue{}},
        {"valueText", JsonValue{}},
    });
}

} // namespace

WidgetSemanticsScope::WidgetSemanticsScope(const RetainedNode& node, const CommandIndex* commands,
                                           const InputRouter* input, std::string default_role)
    : node_(node), commands_(commands), input_(input), explicit_semantics_(property("semantics")),
      role_(std::move(default_role)) {
    if (const auto explicit_role = text(explicit_field("role")); explicit_role.has_value()) {
        role_ = lower_ascii(*explicit_role);
    }
    actions_ = actions_for(role_);
    description_ = text(explicit_field("description"));
    value_text_ = text(explicit_field("valueText"));
    if (const auto label = text(explicit_field("label")); label.has_value()) {
        name_ = *label;
        explicit_name_ = true;
    } else if (const auto explicit_name = text(explicit_field("name")); explicit_name.has_value()) {
        name_ = *explicit_name;
        explicit_name_ = true;
    }
    if (const auto live = text(explicit_field("liveRegion")); live.has_value()) {
        live_region_ = lower_ascii(*live);
    }

    const std::optional<bool> explicit_disabled = boolean(explicit_field("disabled"));
    const std::optional<bool> enabled = boolean(property("enabled"));
    disabled_ = explicit_disabled.value_or(false) || (enabled.has_value() && !*enabled);
    if (!disabled_ && commands_ != nullptr) {
        const std::optional<CommandActivationBinding> binding =
            commands_->activation_binding(node_);
        disabled_ =
            binding.has_value() && binding->command != nullptr && !binding->command->enabled;
    }
    checked_ = boolean(explicit_field("checked"));
    if (!checked_.has_value() && (role_ == "switch" || role_ == "checkbox")) {
        checked_ = boolean(property("checked"));
        if (!checked_.has_value())
            checked_ = boolean(retained("$checked"));
        if (!checked_.has_value())
            checked_ = boolean(property("defaultChecked"));
    }
    const auto semantic_or_property = [this](const std::string_view name) {
        std::optional<bool> value = boolean(explicit_field(name));
        return value.has_value() ? value : boolean(property(name));
    };
    dirty_ = semantic_or_property("dirty");
    expanded_ = semantic_or_property("expanded");
    invalid_ = semantic_or_property("invalid");
    read_only_ = semantic_or_property("readOnly");
    required_ = semantic_or_property("required");
    selected_ = semantic_or_property("selected");
    touched_ = semantic_or_property("touched");
    if (role_ == "text_field" && !read_only_.has_value())
        read_only_ = false;
    if (!selected_.has_value() && (role_ == "row" || role_ == "grid_cell"))
        selected_ = false;

    const runtime::Value* current = explicit_field("value");
    if (current != nullptr && current->number() != nullptr) {
        const runtime::Value* minimum = explicit_field("minimum");
        const runtime::Value* maximum = explicit_field("maximum");
        value_range_ = object({
            {"current", JsonValue(*current->number())},
            {"maximum", maximum != nullptr && maximum->number() != nullptr
                            ? JsonValue(*maximum->number())
                            : JsonValue{}},
            {"minimum", minimum != nullptr && minimum->number() != nullptr
                            ? JsonValue(*minimum->number())
                            : JsonValue{}},
            {"text", nullable_text(value_text_)},
        });
    }

    if (!motion_input_eligible(node_)) {
        disabled_ = true;
        actions_.clear();
    }
}

const RetainedNode& WidgetSemanticsScope::node() const noexcept {
    return node_;
}
const CommandIndex* WidgetSemanticsScope::command_index() const noexcept {
    return commands_;
}

const runtime::ExpressionValue*
WidgetSemanticsScope::expression_property(const std::string_view name) const noexcept {
    const auto found = node_.description().properties.find(name);
    return found != node_.description().properties.end() ? &found->second : nullptr;
}

const runtime::Value* WidgetSemanticsScope::property(const std::string_view name) const noexcept {
    const runtime::ExpressionValue* expression = expression_property(name);
    return expression != nullptr ? expression->data_value() : nullptr;
}

bool WidgetSemanticsScope::has_action(const std::string_view name) const noexcept {
    const auto found = node_.description().properties.find(name);
    return found != node_.description().properties.end() && found->second.action() != nullptr &&
           *found->second.action() != nullptr;
}

const runtime::Value* WidgetSemanticsScope::retained(const std::string_view name) const noexcept {
    return node_.retained_value(name);
}

const std::string* WidgetSemanticsScope::edited_text() const noexcept {
    return input_ != nullptr ? input_->edited_text(node_.identity()) : nullptr;
}

std::vector<WidgetSubtarget> WidgetSemanticsScope::subtargets() const {
    return input_ != nullptr ? input_->subtargets(node_.identity())
                             : std::vector<WidgetSubtarget>{};
}

const NotificationService* WidgetSemanticsScope::notifications() const noexcept {
    return input_ != nullptr ? &input_->notifications() : nullptr;
}

const runtime::Value*
WidgetSemanticsScope::explicit_field(const std::string_view name) const noexcept {
    return explicit_semantics_ != nullptr ? explicit_semantics_->field(name) : nullptr;
}

std::optional<bool> WidgetSemanticsScope::boolean(const runtime::Value* value) const noexcept {
    return value != nullptr && value->boolean() != nullptr ? std::optional<bool>(*value->boolean())
                                                           : std::nullopt;
}

std::optional<std::string> WidgetSemanticsScope::text(const runtime::Value* value) const {
    if (value == nullptr)
        return std::nullopt;
    if (value->string() != nullptr)
        return *value->string();
    if (value->key() != nullptr)
        return value->key()->value;
    return std::nullopt;
}

bool WidgetSemanticsScope::effective_boolean(const std::string_view controlled,
                                             const std::string_view retained_name,
                                             const std::string_view initial,
                                             const bool fallback) const noexcept {
    std::optional<bool> value = boolean(property(controlled));
    if (!value.has_value())
        value = boolean(retained(retained_name));
    if (!value.has_value())
        value = boolean(property(initial));
    return value.value_or(fallback);
}

std::optional<std::string>
WidgetSemanticsScope::effective_text(const std::string_view controlled,
                                     const std::string_view retained_name,
                                     const std::string_view initial) const {
    std::optional<std::string> value = text(property(controlled));
    if (!value.has_value())
        value = text(retained(retained_name));
    if (!value.has_value())
        value = text(property(initial));
    return value;
}

bool WidgetSemanticsScope::has_ancestor(const std::string_view type) const noexcept {
    for (const RetainedNode* current = node_.parent(); current != nullptr;
         current = current->parent()) {
        if (current->description().type == type)
            return true;
    }
    return false;
}

std::string_view WidgetSemanticsScope::role() const noexcept {
    return role_;
}
void WidgetSemanticsScope::role(std::string value) {
    role_ = lower_ascii(std::move(value));
    actions_ = actions_for(role_);
}
void WidgetSemanticsScope::name(std::string value) {
    if (!explicit_name_)
        name_ = std::move(value);
}
void WidgetSemanticsScope::default_name(std::string value) {
    if (!explicit_name_ && name_.empty())
        name_ = std::move(value);
}
void WidgetSemanticsScope::actions(std::vector<std::string> values) {
    actions_ = std::move(values);
}
void WidgetSemanticsScope::live_region(std::string value) {
    live_region_ = lower_ascii(std::move(value));
}
void WidgetSemanticsScope::checked(const std::optional<bool> value) noexcept {
    checked_ = value;
}
void WidgetSemanticsScope::expanded(const std::optional<bool> value) noexcept {
    expanded_ = value;
}
void WidgetSemanticsScope::selected(const std::optional<bool> value) noexcept {
    selected_ = value;
}
void WidgetSemanticsScope::value_text(std::optional<std::string> value) {
    value_text_ = std::move(value);
}
void WidgetSemanticsScope::default_value_text(std::optional<std::string> value) {
    if (!value_text_.has_value())
        value_text_ = std::move(value);
}
void WidgetSemanticsScope::value_range(JsonValue value) {
    value_range_ = std::move(value);
}
void WidgetSemanticsScope::read_only(const std::optional<bool> value) noexcept {
    read_only_ = value;
}
void WidgetSemanticsScope::dirty(const std::optional<bool> value) noexcept {
    dirty_ = value;
}
void WidgetSemanticsScope::invalid(const std::optional<bool> value) noexcept {
    invalid_ = value;
}
void WidgetSemanticsScope::required(const std::optional<bool> value) noexcept {
    required_ = value;
}
void WidgetSemanticsScope::touched(const std::optional<bool> value) noexcept {
    touched_ = value;
}
void WidgetSemanticsScope::input_capabilities(const bool focusable, const bool disabled) {
    disabled_ = disabled_ || disabled;
    if (disabled_) {
        actions_.clear();
        return;
    }
    if (focusable && std::ranges::find(actions_, "focus") == actions_.end()) {
        actions_.emplace_back("focus");
    }
    std::ranges::sort(actions_);
    actions_.erase(std::unique(actions_.begin(), actions_.end()), actions_.end());
}
void WidgetSemanticsScope::virtual_before(JsonValue child) {
    before_.push_back(std::move(child));
}
void WidgetSemanticsScope::virtual_after(JsonValue child) {
    after_.push_back(std::move(child));
}

JsonValue WidgetSemanticsScope::virtual_item(
    const std::size_t path_index, const std::size_t path_base, std::string role, std::string name,
    std::vector<std::string> action_names, const std::optional<bool> checked,
    const std::optional<bool> selected, const bool disabled,
    const std::optional<std::size_t> virtual_index, std::optional<std::string> command_id,
    JsonValue::Array children, std::optional<std::string> structural_parent,
    const std::optional<bool> expanded, std::string live_region,
    const std::optional<std::uint64_t> notification_id, JsonValue value_range,
    std::optional<std::string> value_text) const {
    std::vector<JsonValue> actions;
    actions.reserve(action_names.size());
    for (std::string& action : action_names)
        actions.emplace_back(std::move(action));
    return object({
        {"actions", array(std::move(actions))},
        {"children", array(std::move(children))},
        {"description", JsonValue{}},
        {"key",
         node_.description().key.has_value() ? JsonValue(*node_.description().key) : JsonValue{}},
        {"liveRegion", JsonValue(std::move(live_region))},
        {"name", JsonValue(std::move(name))},
        {"role", JsonValue(std::move(role))},
        {"state", object({
                      {"checked", nullable_boolean(checked)},
                      {"dirty", JsonValue{}},
                      {"disabled", JsonValue(disabled)},
                      {"expanded", nullable_boolean(expanded)},
                      {"invalid", JsonValue{}},
                      {"readOnly", JsonValue{}},
                      {"required", JsonValue{}},
                      {"selected", nullable_boolean(selected)},
                      {"touched", JsonValue{}},
                      {"value", std::move(value_range)},
                      {"valueText", nullable_text(value_text)},
                  })},
        {"structuralPath",
         JsonValue(structural_parent.value_or(std::string(node_.structural_path())) + "/" +
                   std::to_string(path_base + path_index))},
        {"virtualCommandId", command_id.has_value() ? JsonValue(*command_id) : JsonValue{}},
        {"virtualIndex", virtual_index.has_value()
                             ? JsonValue(static_cast<std::int64_t>(*virtual_index))
                             : JsonValue{}},
        {"virtualNotificationId", notification_id.has_value()
                                      ? JsonValue(static_cast<std::int64_t>(*notification_id))
                                      : JsonValue{}},
    });
}

JsonValue WidgetSemanticsScope::virtual_command(const CommandSnapshot& command,
                                                const std::size_t index,
                                                const std::string_view role) const {
    return object({
        {"actions", command.enabled ? array({JsonValue("activate")}) : array()},
        {"children", array()},
        {"description", JsonValue{}},
        {"key",
         node_.description().key.has_value() ? JsonValue(*node_.description().key) : JsonValue{}},
        {"liveRegion", JsonValue("off")},
        {"name", JsonValue(command.label)},
        {"role", JsonValue(std::string(role))},
        {"state", object({
                      {"checked", nullable_boolean(command.checked)},
                      {"dirty", JsonValue{}},
                      {"disabled", JsonValue(!command.enabled)},
                      {"expanded", JsonValue{}},
                      {"invalid", JsonValue{}},
                      {"readOnly", JsonValue{}},
                      {"required", JsonValue{}},
                      {"selected", JsonValue{}},
                      {"touched", JsonValue{}},
                      {"value", JsonValue{}},
                      {"valueText", JsonValue{}},
                  })},
        {"structuralPath", JsonValue(std::string(node_.structural_path()) + "/" +
                                     std::to_string(2'000'000U + index))},
        {"virtualCommandId", JsonValue(command.id)},
        {"virtualIndex", JsonValue{}},
        {"virtualNotificationId", JsonValue{}},
    });
}

JsonValue WidgetSemanticsScope::virtual_tab(const runtime::Value& entry,
                                            const std::string_view selected_id,
                                            const std::size_t index) const {
    const std::string id = text(entry.field("id")).value_or(std::string{});
    const std::string label = text(entry.field("label")).value_or(id);
    std::vector<JsonValue> actions;
    for (std::string action : actions_for("tab"))
        actions.emplace_back(std::move(action));
    return object({
        {"actions", array(std::move(actions))},
        {"children", array()},
        {"description", JsonValue{}},
        {"key",
         node_.description().key.has_value() ? JsonValue(*node_.description().key) : JsonValue{}},
        {"liveRegion", JsonValue("off")},
        {"name", JsonValue(label)},
        {"role", JsonValue("tab")},
        {"state", semantic_state(id == selected_id)},
        {"structuralPath", JsonValue(std::string(node_.structural_path()) + "/" +
                                     std::to_string(2'000'000U + index))},
        {"virtualCommandId", JsonValue{}},
        {"virtualIndex", JsonValue(static_cast<std::int64_t>(index))},
        {"virtualNotificationId", JsonValue{}},
    });
}

JsonValue WidgetSemanticsScope::build(JsonValue::Array retained_children) const {
    JsonValue::Array children;
    children.reserve(before_.size() + retained_children.size() + after_.size());
    children.insert(children.end(), before_.begin(), before_.end());
    children.insert(children.end(), retained_children.begin(), retained_children.end());
    children.insert(children.end(), after_.begin(), after_.end());
    std::vector<JsonValue> actions;
    actions.reserve(actions_.size());
    for (const std::string& action : actions_)
        actions.emplace_back(action);
    return object({
        {"actions", array(std::move(actions))},
        {"children", array(std::move(children))},
        {"description", nullable_text(description_)},
        {"key",
         node_.description().key.has_value() ? JsonValue(*node_.description().key) : JsonValue{}},
        {"liveRegion", JsonValue(live_region_)},
        {"name", JsonValue(name_)},
        {"role", JsonValue(role_)},
        {"state", object({
                      {"checked", nullable_boolean(checked_)},
                      {"dirty", nullable_boolean(dirty_)},
                      {"disabled", JsonValue(disabled_)},
                      {"expanded", nullable_boolean(expanded_)},
                      {"invalid", nullable_boolean(invalid_)},
                      {"readOnly", nullable_boolean(read_only_)},
                      {"required", nullable_boolean(required_)},
                      {"selected", nullable_boolean(selected_)},
                      {"touched", nullable_boolean(touched_)},
                      {"value", value_range_},
                      {"valueText", nullable_text(value_text_)},
                  })},
        {"structuralPath", JsonValue(std::string(node_.structural_path()))},
        {"virtualCommandId", JsonValue{}},
        {"virtualIndex", JsonValue{}},
        {"virtualNotificationId", JsonValue{}},
    });
}

bool widget_semantic_hidden(const RetainedNode& node) {
    const auto found = node.description().properties.find("semantics");
    const runtime::Value* semantics =
        found != node.description().properties.end() ? found->second.value() : nullptr;
    const runtime::Value* decorative =
        semantics != nullptr ? semantics->field("decorative") : nullptr;
    return decorative != nullptr && decorative->boolean() != nullptr && *decorative->boolean();
}

} // namespace strata::ui
