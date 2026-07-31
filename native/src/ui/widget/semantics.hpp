#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data/json.hpp"
#include "ui/command.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {

class InputRouter;
class NotificationService;

/** Mutable typed semantic projection owned for the duration of one node derivation. */
class WidgetSemanticsScope final {
public:
    WidgetSemanticsScope(
        const RetainedNode& node,
        const CommandIndex* commands,
        const InputRouter* input,
        std::string default_role
    );

    [[nodiscard]] const RetainedNode& node() const noexcept;
    [[nodiscard]] const CommandIndex* command_index() const noexcept;
    [[nodiscard]] const runtime::ExpressionValue* expression_property(
        std::string_view name
    ) const noexcept;
    [[nodiscard]] const runtime::Value* property(std::string_view name) const noexcept;
    [[nodiscard]] bool has_action(std::string_view name) const noexcept;
    [[nodiscard]] const runtime::Value* retained(std::string_view name) const noexcept;
    [[nodiscard]] const std::string* edited_text() const noexcept;
    [[nodiscard]] std::vector<WidgetSubtarget> subtargets() const;
    [[nodiscard]] const NotificationService* notifications() const noexcept;
    [[nodiscard]] const runtime::Value* explicit_field(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<bool> boolean(const runtime::Value* value) const noexcept;
    [[nodiscard]] std::optional<std::string> text(const runtime::Value* value) const;
    [[nodiscard]] bool effective_boolean(
        std::string_view controlled,
        std::string_view retained_name,
        std::string_view initial,
        bool fallback
    ) const noexcept;
    [[nodiscard]] std::optional<std::string> effective_text(
        std::string_view controlled,
        std::string_view retained_name,
        std::string_view initial
    ) const;
    [[nodiscard]] bool has_ancestor(std::string_view type) const noexcept;

    [[nodiscard]] std::string_view role() const noexcept;
    void role(std::string value);
    void name(std::string value);
    void default_name(std::string value);
    void actions(std::vector<std::string> values);
    void live_region(std::string value);
    void checked(std::optional<bool> value) noexcept;
    void expanded(std::optional<bool> value) noexcept;
    void selected(std::optional<bool> value) noexcept;
    void value_text(std::optional<std::string> value);
    void default_value_text(std::optional<std::string> value);
    void value_range(data::JsonValue value);
    void read_only(std::optional<bool> value) noexcept;
    void dirty(std::optional<bool> value) noexcept;
    void invalid(std::optional<bool> value) noexcept;
    void required(std::optional<bool> value) noexcept;
    void touched(std::optional<bool> value) noexcept;
    /** Applies the resolved widget-plus-behavior input contract to accessibility actions/state. */
    void input_capabilities(bool focusable, bool disabled);

    void virtual_before(data::JsonValue child);
    void virtual_after(data::JsonValue child);
    [[nodiscard]] data::JsonValue virtual_item(
        std::size_t path_index,
        std::size_t path_base,
        std::string role,
        std::string name,
        std::vector<std::string> actions = {},
        std::optional<bool> checked = std::nullopt,
        std::optional<bool> selected = std::nullopt,
        bool disabled = false,
        std::optional<std::size_t> virtual_index = std::nullopt,
        std::optional<std::string> command_id = std::nullopt,
        data::JsonValue::Array children = {},
        std::optional<std::string> structural_parent = std::nullopt,
        std::optional<bool> expanded = std::nullopt,
        std::string live_region = "off",
        std::optional<std::uint64_t> notification_id = std::nullopt
    ) const;
    [[nodiscard]] data::JsonValue virtual_command(
        const CommandSnapshot& command,
        std::size_t index,
        std::string_view role = "menu_item"
    ) const;
    [[nodiscard]] data::JsonValue virtual_tab(
        const runtime::Value& entry,
        std::string_view selected_id,
        std::size_t index
    ) const;

    [[nodiscard]] data::JsonValue build(data::JsonValue::Array retained_children) const;

private:
    const RetainedNode& node_;
    const CommandIndex* commands_;
    const InputRouter* input_;
    const runtime::Value* explicit_semantics_ = nullptr;
    std::string role_;
    std::string name_;
    bool explicit_name_ = false;
    std::string live_region_ = "off";
    std::vector<std::string> actions_;
    data::JsonValue::Array before_;
    data::JsonValue::Array after_;
    std::optional<std::string> description_;
    std::optional<std::string> value_text_;
    std::optional<bool> checked_;
    std::optional<bool> dirty_;
    bool disabled_ = false;
    std::optional<bool> expanded_;
    std::optional<bool> invalid_;
    std::optional<bool> read_only_;
    std::optional<bool> required_;
    std::optional<bool> selected_;
    std::optional<bool> touched_;
    data::JsonValue value_range_;
};

[[nodiscard]] bool widget_semantic_hidden(const RetainedNode& node);
void register_primitive_widget_semantics(WidgetRegistry& registry);
void register_control_widget_semantics(WidgetRegistry& registry);
void register_shell_widget_semantics(WidgetRegistry& registry);
void register_collection_widget_semantics(WidgetRegistry& registry);

} // namespace strata::ui
