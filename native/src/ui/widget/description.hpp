#pragma once

#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/widget/registry.hpp"

namespace strata::ui {

using WidgetLayoutFields = std::map<std::string, runtime::Value, std::less<>>;

/** Typed layout-default capability used by one widget's describe hook. */
class WidgetLayoutDefaultsScope final {
public:
    explicit WidgetLayoutDefaultsScope(const DescriptionNode::Properties& properties) noexcept;

    [[nodiscard]] const runtime::Value* property(std::string_view name) const noexcept;
    [[nodiscard]] double number(std::string_view name, double fallback) const noexcept;
    [[nodiscard]] bool boolean(std::string_view name, bool fallback) const noexcept;
    [[nodiscard]] std::string string(std::string_view name, std::string fallback = {}) const;

    void set(std::string name, runtime::Value value);
    void padding(double left, double top, double right, double bottom);
    void intrinsic(double width, double height);
    [[nodiscard]] WidgetLayoutFields take();

private:
    const DescriptionNode::Properties& properties_;
    WidgetLayoutFields fields_;
};

/** Typed structural-expansion capability used by one widget's describe hook. */
class WidgetDescriptionScope final {
public:
    WidgetDescriptionScope(
        WidgetDescriptionExpansion& description,
        std::string_view state_scope,
        const runtime::RuntimeActionRegistry& actions,
        const WidgetRegistry& registry,
        const RetainedDescriptionSnapshot::Node* retained,
        WidgetTemplateInstantiator instantiate_template,
        WidgetRetainedDependencyObserver observe_retained
    ) noexcept;

    [[nodiscard]] WidgetDescriptionExpansion& description() noexcept;
    [[nodiscard]] const WidgetDescriptionExpansion& description() const noexcept;
    [[nodiscard]] std::string_view state_scope() const noexcept;
    [[nodiscard]] const runtime::Value* property(std::string_view name) const noexcept;
    [[nodiscard]] const runtime::Value* retained(std::string_view name) const;
    [[nodiscard]] double number(std::string_view name, double fallback) const noexcept;
    [[nodiscard]] bool boolean(std::string_view name, bool fallback) const noexcept;
    [[nodiscard]] std::string string(std::string_view name, std::string fallback = {}) const;
    [[nodiscard]] std::vector<const runtime::Value*> list(std::string_view name) const;
    [[nodiscard]] runtime::ExpressionValue action(std::string_view id) const;
    [[nodiscard]] std::shared_ptr<const runtime::Action> action_contract(
        std::string_view id
    ) const;
    [[nodiscard]] std::shared_ptr<const runtime::ActionValue> bound_action(
        std::string_view property
    ) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> instantiate(
        std::string_view template_property,
        std::string key,
        WidgetTemplateArguments arguments
    ) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> instantiate_component(
        std::string_view component,
        std::string key,
        WidgetTemplateArguments arguments
    ) const;
    /** Builds the common enabled/focus/hover/press projection for authored control chrome. */
    [[nodiscard]] runtime::Value presentation_state(
        WidgetLayoutFields fields = {}
    ) const;
    /** Instantiates, marks, and installs one input/semantics-transparent authored presentation. */
    [[nodiscard]] bool install_presentation(
        std::string_view template_property,
        WidgetTemplateArguments arguments
    );

    void set_layout(DescriptionNode::Properties& properties, std::string name, runtime::Value value) const;
    void set_layout(std::string name, runtime::Value value);
    void apply_layout_defaults(std::string_view type, DescriptionNode::Properties& properties) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> with_layout(
        const std::shared_ptr<const DescriptionNode>& source,
        std::string name,
        runtime::Value value
    ) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> node(
        std::string type,
        std::optional<std::string> key,
        DescriptionNode::Properties properties = {},
        std::vector<std::shared_ptr<const DescriptionNode>> children = {},
        std::vector<DescriptionBehavior> behaviors = {}
    ) const;
    /** Installs a source whose item descriptions are built only when their range materializes. */
    void set_generated_children(
        std::size_t count,
        WidgetGeneratedChildHook factory,
        WidgetGeneratedVirtualization virtualization = {}
    );
    void synthesized(std::size_t count = 1U) noexcept;

private:
    WidgetDescriptionExpansion& description_;
    std::string_view state_scope_;
    const runtime::RuntimeActionRegistry& actions_;
    const WidgetRegistry& registry_;
    const RetainedDescriptionSnapshot::Node* retained_ = nullptr;
    WidgetTemplateInstantiator instantiate_template_;
    WidgetRetainedDependencyObserver observe_retained_;
};

[[nodiscard]] runtime::Value widget_object(
    std::initializer_list<std::pair<std::string, runtime::Value>> fields
);
[[nodiscard]] runtime::Value widget_fill();
[[nodiscard]] const std::string* widget_description_string(const runtime::Value* value) noexcept;
[[nodiscard]] DescriptionNode::Properties widget_transparent_properties();
void widget_mark_native_presentation(DescriptionNode::Properties& properties);
[[nodiscard]] std::shared_ptr<const DescriptionNode> widget_native_presentation(
    const std::shared_ptr<const DescriptionNode>& node
);
[[nodiscard]] bool widget_native_presentation_root(const RetainedNode& node) noexcept;
[[nodiscard]] bool widget_inside_native_presentation(const RetainedNode& node) noexcept;
[[nodiscard]] const RetainedNode* widget_native_input_owner(
    const RetainedNode* node
) noexcept;
[[nodiscard]] RetainedNode* widget_native_input_owner(RetainedNode* node) noexcept;
[[nodiscard]] DescriptionNode::Properties widget_text_properties(
    std::string text,
    runtime::Value layout = runtime::Value{}
);

void register_primitive_widget_descriptions(WidgetRegistry& registry);
void register_control_widget_descriptions(WidgetRegistry& registry);
void register_shell_widget_descriptions(WidgetRegistry& registry);
void register_collection_widget_descriptions(WidgetRegistry& registry);

} // namespace strata::ui
