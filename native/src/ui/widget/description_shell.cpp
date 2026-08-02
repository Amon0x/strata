#include "ui/widget/description.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "runtime/action.hpp"
#include "runtime/registry.hpp"
#include "ui/widget/shell_model.hpp"

namespace strata::ui {
namespace {

void menu_bar_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(30.0));
    scope.set("kind", runtime::Value("ROW"));
    scope.set("width", widget_fill());
}

void toolbar_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(36.0));
    scope.set("kind", runtime::Value("ROW"));
    scope.set("width", widget_fill());
}

void banner_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(46.0));
    scope.set("width", widget_fill());
}

void tooltip_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("PANEL"));
    scope.set("width", runtime::Value("content"));
}

[[nodiscard]] runtime::Value tooltip_portal_layout(
    const DescriptionNode& content
) {
    std::map<std::string, runtime::Value, std::less<>> fields{
        {"anchorAlign", runtime::Value("CENTER")},
        {"anchorFlip", runtime::Value(true)},
        {"anchorGap", runtime::Value(6.0)},
        {"anchorShift", runtime::Value(true)},
        {"anchorSide", runtime::Value("BOTTOM")},
        {"anchorTarget", runtime::Value("parent")},
        {"detachFromParentClip", runtime::Value(true)},
        {"height", runtime::Value("content")},
        {"kind", runtime::Value("PORTAL")},
        {"portalTarget", runtime::Value("root")},
        {"width", runtime::Value("content")},
        {"zIndex", runtime::Value(22'000.0)},
    };
    const auto authored = content.properties.find("$layout");
    const runtime::Value* layout = authored != content.properties.end()
        ? authored->second.data_value()
        : nullptr;
    for (const std::string_view name : {
             "anchorAlign",
             "anchorFlip",
             "anchorGap",
             "anchorShift",
             "anchorSide",
             "matchAnchorWidth",
         }) {
        if (const runtime::Value* value = layout != nullptr ? layout->field(name) : nullptr;
            value != nullptr) {
            fields.insert_or_assign(std::string(name), *value);
        }
    }
    std::vector<std::pair<std::string, runtime::Value>> values;
    values.reserve(fields.size());
    for (auto& [name, value] : fields) {
        values.emplace_back(std::move(name), std::move(value));
    }
    return runtime::Value(std::move(values));
}

void tooltip_expand(WidgetDescriptionScope& scope) {
    const std::string* component =
        widget_description_string(scope.property("contentTemplate"));
    if (component == nullptr) return;
    bool visible = false;
    if (const runtime::Value* controlled = scope.property("visible");
        controlled != nullptr && controlled->boolean() != nullptr) {
        visible = *controlled->boolean();
    } else if (const runtime::Value* retained = scope.retained(tooltip_shown_state);
               retained != nullptr && retained->boolean() != nullptr) {
        visible = *retained->boolean();
    }
    if (!visible) return;
    WidgetDescriptionExpansion& description = scope.description();
    const std::string key = description.key.value_or("$tooltip");
    const std::string text = scope.string("text");
    std::shared_ptr<const DescriptionNode> content = scope.instantiate_component(
        *component,
        key + ".content",
        WidgetTemplateArguments{
            {"key", runtime::Value(runtime::KeyValue{key + ".content"})},
            {"text", runtime::Value(text)},
        }
    );
    if (content == nullptr) return;
    DescriptionNode::Properties portal = widget_transparent_properties();
    widget_mark_native_presentation(portal);
    portal.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(tooltip_portal_layout(*content))
    );
    description.children.push_back(scope.node(
        "Panel",
        key + ".popup",
        std::move(portal),
        {std::move(content)}
    ));
    scope.synthesized();
}

void banner_expand(WidgetDescriptionScope& scope) {
    const runtime::Value* dismissed = scope.retained("$dismissed");
    if (dismissed == nullptr || dismissed->boolean() == nullptr || !*dismissed->boolean()) return;
    scope.description().properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(widget_object({
            {"height", runtime::Value(0.0)},
            {"kind", runtime::Value("SPACER")},
            {"width", runtime::Value(0.0)},
        }))
    );
    scope.description().properties.erase("action");
}

void breadcrumbs_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(30.0));
    scope.set("width", widget_fill());
}

void chip_input_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(40.0));
    scope.set("width", widget_fill());
}

void modal_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", widget_fill());
    scope.set("kind", runtime::Value("OVERLAY"));
    scope.set("width", widget_fill());
    scope.set("zIndex", runtime::Value(10'000.0));
}

void modal_expand(WidgetDescriptionScope& scope) {
    if (!scope.boolean("open", true)) {
        scope.description().properties.insert_or_assign(
            "$layout",
            runtime::ExpressionValue(widget_object({{"kind", runtime::Value("SPACER")}}))
        );
        scope.description().properties.erase("onDismiss");
        scope.description().children.clear();
        return;
    }
    if (!scope.boolean("dismissible", true) || scope.description().properties.contains("onDismiss")) {
        return;
    }
    runtime::ExpressionValue dismiss = scope.action("modal-dismiss");
    if (dismiss.action() != nullptr && *dismiss.action() != nullptr) {
        scope.description().properties.emplace("onDismiss", std::move(dismiss));
    }
}

void split_pane_defaults(WidgetLayoutDefaultsScope& scope) {
    const bool vertical = scope.string("axis") == "VERTICAL";
    scope.set("height", widget_fill());
    scope.set("kind", runtime::Value(vertical ? "COLUMN" : "ROW"));
    scope.set("width", widget_fill());
}

void form_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("gap", runtime::Value(8.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
}

std::shared_ptr<const runtime::ActionValue> form_submit_request(
    const RetainedNode& form,
    const runtime::RuntimeActionRegistry& actions
) {
    if (!form.description().key.has_value()) return nullptr;
    const std::shared_ptr<const runtime::ActionContract> contract = actions.contract("form.submit");
    if (contract == nullptr) return nullptr;
    runtime::Value payload(std::vector<std::pair<std::string, runtime::Value>>{
        {"key", runtime::Value(runtime::KeyValue{*form.description().key})},
    });
    payload = actions.decode_payload("form.submit", std::move(payload));
    return std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
        std::make_shared<const runtime::Action>(contract, std::move(payload)),
        std::nullopt,
        {},
    });
}

void field_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("gap", runtime::Value(4.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
    scope.padding(0.0, 0.0, 0.0, 16.0);
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> with_field_capabilities(
    const std::shared_ptr<const DescriptionNode>& source,
    const bool enabled,
    const bool read_only
) {
    if (source == nullptr || (enabled && !read_only)) return source;
    auto result = std::make_shared<DescriptionNode>(*source);
    if (!enabled) {
        result->properties.insert_or_assign(
            "enabled", runtime::ExpressionValue(runtime::Value(false))
        );
    }
    if (read_only) {
        result->properties.insert_or_assign(
            "readOnly", runtime::ExpressionValue(runtime::Value(true))
        );
    }
    if (source->children == nullptr || source->children->size() == 0U) return result;

    if (dynamic_cast<const GeneratedDescriptionChildren*>(source->children.get()) != nullptr) {
        const std::shared_ptr<const DescriptionChildren> children = source->children;
        result->children = std::make_shared<const GeneratedDescriptionChildren>(
            children->size(),
            [children, enabled, read_only](const std::size_t index) {
                return with_field_capabilities(children->at(index), enabled, read_only);
            }
        );
        return result;
    }

    std::vector<std::shared_ptr<const DescriptionNode>> children;
    children.reserve(source->children->size());
    for (std::size_t index = 0U; index < source->children->size(); ++index) {
        children.push_back(with_field_capabilities(
            source->children->at(index), enabled, read_only
        ));
    }
    result->children = std::make_shared<const EagerDescriptionChildren>(std::move(children));
    return result;
}

void hidden_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(0.0));
    scope.set("width", runtime::Value(0.0));
}

void split_pane_expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    if (description.children.size() != 2U || !description.key.has_value()) {
        throw std::logic_error("validated SplitPane must contain exactly two children and a key");
    }
    const bool vertical = scope.string("axis") == "VERTICAL";
    const runtime::Value* controlled_ratio = scope.property("ratio");
    const runtime::Value* retained_ratio = scope.retained("strata.gesture.splitRatio");
    const double ratio = controlled_ratio != nullptr && controlled_ratio->number() != nullptr
        ? *controlled_ratio->number()
        : retained_ratio != nullptr && retained_ratio->number() != nullptr
            ? *retained_ratio->number()
            : scope.number("defaultRatio", 0.5);
    const double divider_size = scope.number("dividerSize", 6.0);
    const std::string main_dimension = vertical ? "height" : "width";
    const std::string cross_dimension = vertical ? "width" : "height";
    description.children[0] = scope.with_layout(
        description.children[0],
        main_dimension,
        widget_object({{"weight", runtime::Value(ratio)}})
    );
    description.children[1] = scope.with_layout(
        description.children[1],
        main_dimension,
        widget_object({{"weight", runtime::Value(1.0 - ratio)}})
    );
    DescriptionNode::Properties divider_properties{
        {
            "$layout",
            runtime::ExpressionValue(widget_object({
                {cross_dimension, widget_object({{"weight", runtime::Value(1.0)}})},
                {main_dimension, runtime::Value(divider_size)},
            }))
        },
        {
            "background",
            runtime::ExpressionValue(runtime::Value(runtime::ColorValue{92U, 102U, 118U, 180U}))
        },
        {"border", runtime::ExpressionValue(runtime::Value{})},
        {
            "focusRing",
            runtime::ExpressionValue(widget_object({
                {"color", runtime::Value(runtime::ColorValue{112U, 170U, 250U, 255U})},
                {"inside", runtime::Value(true)},
                {"width", runtime::Value(2.0)},
            }))
        },
        {
            "hoverOverlay",
            runtime::ExpressionValue(runtime::Value(runtime::ColorValue{112U, 155U, 255U, 80U}))
        },
        {"radius", runtime::ExpressionValue(runtime::Value(0.0))},
    };
    std::shared_ptr<const runtime::ActionValue> split_binding = scope.bound_action("onChange");
    std::shared_ptr<const runtime::Action> split_action;
    if (split_binding == nullptr) split_action = scope.action_contract("split-change");
    if (split_binding == nullptr && split_action != nullptr) {
        split_binding = std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
            std::move(split_action), std::nullopt, {},
        });
    }
    std::vector<DescriptionBehavior> divider_behaviors{
        DescriptionBehavior{
            "strata.split-handle",
            true,
            widget_object({{"paneKey", runtime::Value(runtime::KeyValue{*description.key})}}),
            std::move(split_binding),
        },
    };
    description.children.insert(description.children.begin() + 1, scope.node(
        "Panel",
        *description.key + ".divider",
        std::move(divider_properties),
        {},
        std::move(divider_behaviors)
    ));
    scope.synthesized();
}

void field_expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    const bool enabled = scope.boolean("enabled", true);
    const bool read_only = scope.boolean("readOnly", false);
    if (!enabled || read_only) {
        for (std::shared_ptr<const DescriptionNode>& child : description.children) {
            child = with_field_capabilities(child, enabled, read_only);
        }
    }

    std::string label = scope.string("label");
    if (scope.boolean("required", false)) label += " *";
    DescriptionNode::Properties properties = widget_text_properties(std::move(label));
    properties.emplace("fontSize", runtime::ExpressionValue(runtime::Value(13.0)));
    description.children.insert(
        description.children.begin(),
        scope.node("Text", std::nullopt, std::move(properties))
    );

    const std::string* controlled_error = widget_description_string(scope.property("error"));
    const std::string* retained_error = widget_description_string(
        scope.retained("strata.form.error")
    );
    const std::string* visible_error = controlled_error != nullptr && !controlled_error->empty()
                                           ? controlled_error
                                           : retained_error;
    std::string supporting_text;
    bool invalid = false;
    if (visible_error != nullptr && !visible_error->empty()) {
        supporting_text = *visible_error;
        invalid = true;
    } else {
        supporting_text = scope.string("help");
    }
    if (!supporting_text.empty()) {
        DescriptionNode::Properties supporting = widget_text_properties(
            std::move(supporting_text)
        );
        supporting.emplace("fontSize", runtime::ExpressionValue(runtime::Value(12.0)));
        supporting.emplace(
            "color",
            runtime::ExpressionValue(
                invalid
                    ? runtime::Value(runtime::ColorValue{248U, 113U, 113U, 255U})
                    : runtime::Value(runtime::ColorValue{160U, 168U, 178U, 255U})
            )
        );
        description.children.push_back(
            scope.node("Text", std::nullopt, std::move(supporting))
        );
        scope.synthesized(2U);
    } else {
        scope.synthesized();
    }
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const WidgetLayoutDefaultsHook defaults = nullptr,
    const WidgetDescriptionHook expand = nullptr,
    const bool participates = true,
    std::string implicit_key_prefix = {},
    std::string default_action = {},
    WidgetDefaultActionFactory default_action_factory = nullptr
) {
    WidgetDescribePhase phase{
        defaults,
        expand,
        {},
        std::move(default_action),
        participates,
    };
    phase.implicit_key_prefix = std::move(implicit_key_prefix);
    phase.default_action_factory = std::move(default_action_factory);
    registry.register_describe_phase(
        std::move(type),
        std::move(phase)
    );
}

} // namespace

void register_shell_widget_descriptions(WidgetRegistry& registry) {
    add(registry, "MenuBar", &menu_bar_defaults);
    add(registry, "Toolbar", &toolbar_defaults);
    add(registry, "Banner", &banner_defaults, &banner_expand);
    add(registry, "Breadcrumbs", &breadcrumbs_defaults);
    add(registry, "ChipInput", &chip_input_defaults);
    add(registry, "Modal", &modal_defaults, &modal_expand);
    add(registry, "SplitPane", &split_pane_defaults, &split_pane_expand, true, "dsl.split.");
    add(registry, "Form", &form_defaults, nullptr, true, "dsl.form.", {}, &form_submit_request);
    add(registry, "Field", &field_defaults, &field_expand, true, "dsl.field.");
    add(registry, "Command", &hidden_defaults, nullptr, false);
    add(registry, "CommandPalette", &hidden_defaults);
    add(registry, "ToastRegion", &hidden_defaults);
    add(registry, "Tooltip", &tooltip_defaults, &tooltip_expand);

    WidgetCommandPhase command;
    command.declaration = true;
    command.references_self = true;
    command.references_property.clear();
    registry.register_command_phase("Command", std::move(command));

    WidgetCommandPhase command_surface;
    command_surface.references_property = "commands";
    command_surface.all_when_unreferenced = true;
    registry.register_command_phase("MenuBar", command_surface);
    registry.register_command_phase("Toolbar", command_surface);
    registry.register_command_phase("CommandPalette", std::move(command_surface));
}

} // namespace strata::ui
