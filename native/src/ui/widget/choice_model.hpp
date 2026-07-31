#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "ui/tree.hpp"

namespace strata::ui {

[[nodiscard]] inline const runtime::Value* choice_property(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] inline const std::string* choice_id(
    const runtime::Value* value
) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

[[nodiscard]] inline bool choice_is_controlled(const RetainedNode& node) noexcept {
    return choice_id(choice_property(node, "selectedId")) != nullptr;
}

[[nodiscard]] inline const runtime::ValueList* choice_options(
    const RetainedNode& node
) noexcept {
    const runtime::Value* value = choice_property(
        node, node.description().type == "Tabs" ? "tabs" : "options"
    );
    return value != nullptr ? value->list() : nullptr;
}

[[nodiscard]] inline const std::string* choice_option_id(
    const runtime::Value& option
) noexcept {
    const std::string* id = choice_id(option.field("id"));
    return id != nullptr && !id->empty() ? id : nullptr;
}

[[nodiscard]] inline bool choice_option_enabled(const runtime::Value& option) noexcept {
    const runtime::Value* enabled = option.field("enabled");
    return enabled == nullptr || enabled->boolean() == nullptr || *enabled->boolean();
}

struct EffectiveChoice final {
    std::string id;
    std::size_t index = 0U;
};

[[nodiscard]] inline std::optional<EffectiveChoice> find_choice(
    const runtime::ValueList& options,
    const runtime::Value* candidate
) {
    const std::string* candidate_id = choice_id(candidate);
    if (candidate_id == nullptr) return std::nullopt;
    for (std::size_t index = 0U; index < options.values.size(); ++index) {
        const std::string* option_id = choice_option_id(options.values[index]);
        if (option_id != nullptr && *option_id == *candidate_id) {
            return EffectiveChoice{*option_id, index};
        }
    }
    return std::nullopt;
}

/**
 * Canonical controlled -> retained -> authored-default resolution. Candidates that no longer
 * exist are ignored. Tabs fall back to the first identified tab; Select and RadioGroup prefer
 * the first enabled identified option, then the first identified option when all are disabled.
 */
[[nodiscard]] inline std::optional<EffectiveChoice> effective_choice(
    const RetainedNode& node
) {
    const runtime::ValueList* options = choice_options(node);
    if (options == nullptr || options->values.empty()) return std::nullopt;
    for (const runtime::Value* candidate : {
             choice_property(node, "selectedId"),
             node.retained_value("$selectedId"),
             choice_property(node, "defaultSelectedId"),
         }) {
        if (std::optional<EffectiveChoice> result = find_choice(*options, candidate);
            result.has_value()) {
            return result;
        }
    }

    if (node.description().type != "Tabs") {
        for (std::size_t index = 0U; index < options->values.size(); ++index) {
            const std::string* id = choice_option_id(options->values[index]);
            if (id != nullptr && choice_option_enabled(options->values[index])) {
                return EffectiveChoice{*id, index};
            }
        }
    }
    for (std::size_t index = 0U; index < options->values.size(); ++index) {
        const std::string* id = choice_option_id(options->values[index]);
        if (id != nullptr) return EffectiveChoice{*id, index};
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string effective_choice_id(const RetainedNode& node) {
    const std::optional<EffectiveChoice> result = effective_choice(node);
    return result.has_value() ? result->id : std::string{};
}

} // namespace strata::ui
