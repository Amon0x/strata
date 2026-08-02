#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/input.hpp"

namespace strata::ui::input_detail {

using data::JsonValue;

[[nodiscard]] inline JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] inline JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] inline std::string_view policy_name(const runtime::ActionDispatchPolicy policy) noexcept {
    switch (policy) {
    case runtime::ActionDispatchPolicy::required: return "required";
    case runtime::ActionDispatchPolicy::optional: return "optional";
    case runtime::ActionDispatchPolicy::broadcast: return "broadcast";
    case runtime::ActionDispatchPolicy::forwarded: return "forwarded";
    case runtime::ActionDispatchPolicy::framework: return "framework";
    }
    return "required";
}

[[nodiscard]] inline std::string_view status_name(const runtime::ActionDispatchStatus status) noexcept {
    switch (status) {
    case runtime::ActionDispatchStatus::no_action: return "no_action";
    case runtime::ActionDispatchStatus::handled: return "handled";
    case runtime::ActionDispatchStatus::forwarded: return "forwarded";
    case runtime::ActionDispatchStatus::ignored: return "ignored";
    case runtime::ActionDispatchStatus::unhandled: return "unhandled";
    case runtime::ActionDispatchStatus::failed: return "failed";
    }
    return "failed";
}

[[nodiscard]] inline std::string_view state_operation(const std::string_view id) noexcept {
    if (id == "state.set" || id == "state.setFromEvent") return "set";
    if (id == "state.toggle") return "toggle";
    if (id == "state.adjust") return "adjust";
    if (id == "state.reset") return "reset";
    if (id == "state.listAppend") return "list_append";
    if (id == "state.listInsert") return "list_insert";
    if (id == "state.listRemoveValue") return "list_remove_value";
    if (id == "state.listRemoveAt") return "list_remove_at";
    if (id == "state.listToggle") return "list_toggle";
    if (id == "state.listClear") return "list_clear";
    if (id == "state.recordSet") return "record_set";
    return "set";
}

[[nodiscard]] inline std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<char>(character - 'A' + 'a')
                   : static_cast<char>(character);
    });
    return value;
}

[[nodiscard]] inline double number_value(const runtime::Value* value, const double fallback) noexcept {
    return value != nullptr && value->number() != nullptr ? *value->number() : fallback;
}

[[nodiscard]] inline bool boolean_value(const runtime::Value* value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] inline const runtime::Value* scalar_property(
    const RetainedNode& node,
    const std::string_view name
) {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] inline bool native_presentation_root(const RetainedNode& node) noexcept {
    const runtime::Value* marker = scalar_property(node, "$nativePresentation");
    return marker != nullptr && marker->boolean() != nullptr && *marker->boolean();
}

[[nodiscard]] inline bool inside_native_presentation(const RetainedNode& node) noexcept {
    for (const RetainedNode* current = &node;
         current != nullptr;
         current = current->parent()) {
        if (native_presentation_root(*current)) return true;
    }
    return false;
}

[[nodiscard]] inline JsonValue origin(const runtime::ActionOrigin& value) {
    const auto nullable_number = [](const std::optional<std::uint32_t> number) {
        return number.has_value()
                   ? JsonValue(static_cast<std::int64_t>(*number))
                   : JsonValue{};
    };
    return object({
        {"column", nullable_number(value.column)},
        {"componentPath", value.component_path.has_value() ? JsonValue(*value.component_path) : JsonValue{}},
        {"endColumn", nullable_number(value.end_column)},
        {"endLine", nullable_number(value.end_line)},
        {"line", nullable_number(value.line)},
        {"sourceId", JsonValue(value.source_id)},
    });
}

[[nodiscard]] inline JsonValue no_action_outcome(const JsonValue& event) {
    return object({
        {"actionId", JsonValue{}},
        {"event", event},
        {"handlerOwners", array()},
        {"message", JsonValue{}},
        {"status", JsonValue("no_action")},
    });
}

[[nodiscard]] inline const std::string* string_value(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    if (value->key() != nullptr) return &value->key()->value;
    return nullptr;
}

[[nodiscard]] inline bool blank(const std::string_view value) noexcept {
    return std::ranges::all_of(value, [](const unsigned char character) {
        return std::isspace(character) != 0;
    });
}

inline void collect_type(RetainedNode& root, const std::string_view type, std::vector<RetainedNode*>& result) {
    for (const auto& child : root.children()) {
        if (child->description().type == type) result.push_back(child.get());
        collect_type(*child, type, result);
    }
}


} // namespace strata::ui::input_detail
