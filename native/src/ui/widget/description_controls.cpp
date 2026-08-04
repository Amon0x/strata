#include "ui/widget/description.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/widget/choice_model.hpp"

namespace strata::ui {
namespace {

void icon_button_defaults(WidgetLayoutDefaultsScope& scope) {
    if (scope.property("presentationTemplate") != nullptr) {
        scope.set("height", runtime::Value("content"));
        scope.set("width", runtime::Value("content"));
        scope.padding(0.0, 0.0, 0.0, 0.0);
        return;
    }
    scope.set("height", runtime::Value(32.0));
    scope.set("width", runtime::Value(32.0));
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.padding(10.0, 6.0, 10.0, 6.0);
}

void icon_button_expand(WidgetDescriptionScope& scope) {
    const runtime::Value* icon = scope.property("icon");
    const runtime::Value* source = scope.property("source");
    static_cast<void>(scope.install_presentation(
        "presentationTemplate",
        WidgetTemplateArguments{
            {"label", runtime::Value(scope.string("label"))},
            {"icon", icon != nullptr ? *icon : runtime::Value{}},
            {"source", source != nullptr ? *source : runtime::Value{}},
            {"control", scope.presentation_state()},
        }
    ));
}

void checkbox_defaults(WidgetLayoutDefaultsScope& scope) {
    if (scope.property("presentationTemplate") != nullptr) {
        scope.set("height", runtime::Value("content"));
        scope.set("width", runtime::Value("content"));
        scope.padding(0.0, 0.0, 0.0, 0.0);
        return;
    }
    scope.set("height", runtime::Value(24.0));
    scope.set("width", runtime::Value("content"));
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.padding(24.0, 4.0, 4.0, 4.0);
    scope.intrinsic(16.0, 16.0);
}

[[nodiscard]] bool boolean_control_checked(WidgetDescriptionScope& scope) {
    if (const runtime::Value* value = scope.property("checked");
        value != nullptr && value->boolean() != nullptr) {
        return *value->boolean();
    }
    if (const runtime::Value* value = scope.retained("$checked");
        value != nullptr && value->boolean() != nullptr) {
        return *value->boolean();
    }
    return scope.boolean("defaultChecked", false);
}

[[nodiscard]] double effective_number(
    WidgetDescriptionScope& scope,
    const std::string_view controlled,
    const std::string_view retained,
    const std::string_view initial,
    const double fallback
) {
    for (const runtime::Value* candidate : {
             scope.property(controlled),
             scope.retained(retained),
             scope.property(initial),
         }) {
        if (candidate != nullptr && candidate->number() != nullptr &&
            std::isfinite(*candidate->number())) {
            return *candidate->number();
        }
    }
    return fallback;
}

void boolean_control_expand(
    WidgetDescriptionScope& scope
) {
    static_cast<void>(scope.install_presentation(
        "presentationTemplate",
        WidgetTemplateArguments{
            {"label", runtime::Value(scope.string("label"))},
            {"description", runtime::Value(scope.string("description"))},
            {"control",
             scope.presentation_state(WidgetLayoutFields{
                 {"checked", runtime::Value(boolean_control_checked(scope))},
             })},
        }
    ));
}

void checkbox_expand(WidgetDescriptionScope& scope) {
    boolean_control_expand(scope);
}

void toggle_defaults(WidgetLayoutDefaultsScope& scope) {
    if (scope.property("presentationTemplate") != nullptr) {
        scope.set("height", runtime::Value("content"));
        scope.set("width", runtime::Value("content"));
        scope.padding(0.0, 0.0, 0.0, 0.0);
        return;
    }
    scope.set("height", runtime::Value(28.0));
    scope.set("width", runtime::Value("content"));
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.padding(50.0, 4.0, 6.0, 4.0);
    scope.intrinsic(44.0, 20.0);
}

void toggle_expand(WidgetDescriptionScope& scope) {
    boolean_control_expand(scope);
}

void slider_defaults(WidgetLayoutDefaultsScope& scope) {
    const bool vertical = scope.string("axis") == "VERTICAL";
    if (scope.property("presentationTemplate") != nullptr) {
        scope.set("height", runtime::Value("content"));
        scope.set("width", runtime::Value("content"));
        scope.padding(0.0, 0.0, 0.0, 0.0);
        return;
    }
    scope.set("height", runtime::Value(vertical ? 160.0 : 24.0));
    scope.set("width", runtime::Value(vertical ? 24.0 : 160.0));
    scope.padding(6.0, 6.0, 6.0, 6.0);
    scope.intrinsic(vertical ? 24.0 : 160.0, vertical ? 160.0 : 24.0);
}

void slider_expand(WidgetDescriptionScope& scope) {
    const double minimum = scope.number("min", 0.0);
    const double maximum = scope.number("max", 1.0);
    const double value =
        effective_number(scope, "value", "$value", "defaultValue", minimum);
    const double fraction = maximum > minimum
        ? std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0)
        : 0.0;
    const runtime::Value* step = scope.property("step");
    static_cast<void>(scope.install_presentation(
        "presentationTemplate",
        WidgetTemplateArguments{
            {"valueLabel", runtime::Value(scope.string("valueLabel"))},
            {"control", scope.presentation_state(WidgetLayoutFields{
                {"axis", runtime::Value(scope.string("axis", "HORIZONTAL"))},
                {"fraction", runtime::Value(fraction)},
                {"maximum", runtime::Value(maximum)},
                {"minimum", runtime::Value(minimum)},
                {"step", step != nullptr ? *step : runtime::Value{}},
                {"value", runtime::Value(value)},
            })},
        }
    ));
}

void text_box_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(32.0));
    scope.set("width", runtime::Value(180.0));
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.padding(10.0, 6.0, 10.0, 6.0);
}

void text_area_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(96.0));
    scope.set("width", runtime::Value(220.0));
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.padding(8.0, 8.0, 8.0, 8.0);
}

void number_field_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(32.0));
    scope.set("width", runtime::Value(150.0));
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.padding(10.0, 6.0, 10.0, 6.0);
}

void progress_defaults(WidgetLayoutDefaultsScope& scope) {
    if (scope.property("presentationTemplate") != nullptr) {
        scope.set("height", runtime::Value("content"));
        scope.set("width", runtime::Value("content"));
        scope.padding(0.0, 0.0, 0.0, 0.0);
        return;
    }
    scope.set("height", runtime::Value(12.0));
    scope.set("width", runtime::Value(120.0));
    scope.intrinsic(120.0, 12.0);
}

void progress_expand(WidgetDescriptionScope& scope) {
    const double minimum = scope.number("min", 0.0);
    const double maximum = scope.number("max", 1.0);
    const double value = scope.number("value", minimum);
    const double fraction = maximum > minimum
        ? std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0)
        : 0.0;
    static_cast<void>(scope.install_presentation(
        "presentationTemplate",
        WidgetTemplateArguments{
            {"control", scope.presentation_state(WidgetLayoutFields{
                {"fraction", runtime::Value(fraction)},
                {"indeterminate", runtime::Value(scope.boolean("indeterminate", false))},
                {"maximum", runtime::Value(maximum)},
                {"minimum", runtime::Value(minimum)},
                {"value", runtime::Value(value)},
            })},
        }
    ));
    if (!scope.boolean("indeterminate", false) || scope.property("animation") != nullptr) return;
    scope.description().properties.insert_or_assign(
        "$timeline",
        runtime::ExpressionValue(widget_object({
            {"affectsLayout", runtime::Value(false)},
            {"durationNanos", runtime::Value(1'200'000'000.0)},
            {"id", runtime::Value("strata.progress.indeterminate")},
            {"loop", runtime::Value(true)},
            {"running", runtime::Value(true)},
        }))
    );
}

void tabs_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(34.0));
    scope.set("width", widget_fill());
    scope.intrinsic(160.0, 34.0);
}

void select_defaults(WidgetLayoutDefaultsScope& scope) {
    if (scope.property("triggerTemplate") != nullptr) {
        scope.set("height", runtime::Value("content"));
        scope.set("width", runtime::Value("content"));
        scope.padding(0.0, 0.0, 0.0, 0.0);
        return;
    }
    scope.set("height", runtime::Value(32.0));
    scope.set("width", runtime::Value(180.0));
    scope.padding(10.0, 6.0, 10.0, 6.0);
}

[[nodiscard]] bool select_expanded(WidgetDescriptionScope& scope) {
    if (const runtime::Value* value = scope.property("expanded");
        value != nullptr && value->boolean() != nullptr) {
        return *value->boolean();
    }
    if (const runtime::Value* value = scope.retained("$expanded");
        value != nullptr && value->boolean() != nullptr) {
        return *value->boolean();
    }
    if (const runtime::Value* value = scope.property("defaultExpanded");
        value != nullptr && value->boolean() != nullptr) {
        return *value->boolean();
    }
    return false;
}

[[nodiscard]] std::optional<std::size_t> selected_option(
    WidgetDescriptionScope& scope,
    const std::vector<const runtime::Value*>& options
) {
    const runtime::Value* retained = scope.retained("$selectedId");
    for (const runtime::Value* candidate : {
             scope.property("selectedId"),
             retained,
             scope.property("defaultSelectedId"),
         }) {
        const std::string* id = widget_description_string(candidate);
        if (id == nullptr) continue;
        for (std::size_t index = 0U; index < options.size(); ++index) {
            const std::string* option_id =
                widget_description_string(options[index]->field("id"));
            if (option_id != nullptr && *option_id == *id) return index;
        }
    }
    for (std::size_t index = 0U; index < options.size(); ++index) {
        const std::string* id = widget_description_string(options[index]->field("id"));
        const runtime::Value* enabled = options[index]->field("enabled");
        if (id != nullptr &&
            (enabled == nullptr || enabled->boolean() == nullptr || *enabled->boolean())) {
            return index;
        }
    }
    return options.empty() ? std::nullopt : std::optional<std::size_t>(0U);
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> append_popup_children(
    const std::shared_ptr<const DescriptionNode>& source,
    std::vector<std::shared_ptr<const DescriptionNode>> children
) {
    auto result = std::make_shared<DescriptionNode>(*source);
    std::vector<std::shared_ptr<const DescriptionNode>> merged;
    if (source->children != nullptr) {
        merged.reserve(source->children->size() + children.size());
        for (std::size_t index = 0U; index < source->children->size(); ++index) {
            merged.push_back(source->children->at(index));
        }
    }
    merged.insert(
        merged.end(),
        std::make_move_iterator(children.begin()),
        std::make_move_iterator(children.end())
    );
    result->children = std::make_shared<const EagerDescriptionChildren>(std::move(merged));
    return result;
}

[[nodiscard]] runtime::Value select_portal_layout(
    const DescriptionNode& popup,
    const std::string& anchor
) {
    std::map<std::string, runtime::Value, std::less<>> fields{
        {"anchorAlign", runtime::Value("START")},
        {"anchorFlip", runtime::Value(true)},
        {"anchorGap", runtime::Value(4.0)},
        {"anchorShift", runtime::Value(true)},
        {"anchorSide", runtime::Value("BOTTOM")},
        {"anchorTarget", runtime::Value(anchor)},
        {"detachFromParentClip", runtime::Value(true)},
        {"height", runtime::Value("content")},
        {"kind", runtime::Value("PORTAL")},
        {"matchAnchorWidth", runtime::Value(false)},
        {"portalTarget", runtime::Value("root")},
        {"width", runtime::Value("content")},
        {"zIndex", runtime::Value(20'000.0)},
    };
    const auto authored = popup.properties.find("$layout");
    const runtime::Value* layout = authored != popup.properties.end()
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

void select_expand(WidgetDescriptionScope& scope) {
    const std::string* trigger_component =
        widget_description_string(scope.property("triggerTemplate"));
    const std::string* popup_component =
        widget_description_string(scope.property("popupTemplate"));
    const std::string* item_component =
        widget_description_string(scope.property("itemTemplate"));
    const bool authored_popup = popup_component != nullptr && item_component != nullptr;
    if (trigger_component == nullptr && !authored_popup) {
        return;
    }
    WidgetDescriptionExpansion& description = scope.description();
    const std::string key = description.key.value_or("$select");
    const std::vector<const runtime::Value*> options = scope.list("options");
    const std::optional<std::size_t> selected = selected_option(scope, options);
    const bool expanded = select_expanded(scope);
    const bool enabled = scope.boolean("enabled", true);
    const runtime::Value* selected_value = selected.has_value()
        ? options[*selected]
        : nullptr;
    const std::string selected_id = selected_value != nullptr &&
            widget_description_string(selected_value->field("id")) != nullptr
        ? *widget_description_string(selected_value->field("id"))
        : std::string{};
    const std::string selected_label = selected_value != nullptr &&
            widget_description_string(selected_value->field("label")) != nullptr
        ? *widget_description_string(selected_value->field("label"))
        : selected_id;
    if (trigger_component != nullptr) {
        std::shared_ptr<const DescriptionNode> trigger = scope.instantiate_component(
            *trigger_component,
            key + ".trigger",
            WidgetTemplateArguments{
                {"key", runtime::Value(runtime::KeyValue{key + ".trigger"})},
                {"label", runtime::Value(selected_label)},
                {"value", runtime::Value(selected_id)},
                {"enabled", runtime::Value(enabled)},
                {"expanded", runtime::Value(expanded)},
            }
        );
        if (trigger == nullptr) return;
        trigger = widget_native_presentation(trigger);
        description.children = {std::move(trigger)};
    }
    if (!expanded || !authored_popup) return;

    std::vector<std::shared_ptr<const DescriptionNode>> rows;
    rows.reserve(options.size());
    const runtime::Value* active_value = scope.retained("$choiceIndex");
    const std::optional<std::size_t> active =
        active_value != nullptr && active_value->number() != nullptr &&
                *active_value->number() >= 0.0
            ? std::optional<std::size_t>(
                  static_cast<std::size_t>(*active_value->number())
              )
            : selected;
    for (std::size_t index = 0U; index < options.size(); ++index) {
        const runtime::Value& option = *options[index];
        const std::string* id = widget_description_string(option.field("id"));
        if (id == nullptr || id->empty()) continue;
        const std::string* label = widget_description_string(option.field("label"));
        const runtime::Value* option_enabled = option.field("enabled");
        const bool row_enabled = enabled &&
            (option_enabled == nullptr || option_enabled->boolean() == nullptr ||
             *option_enabled->boolean());
        const std::string row_key = choice_option_key(key, *id);
        std::shared_ptr<const DescriptionNode> row = scope.instantiate_component(
            *item_component,
            row_key,
            WidgetTemplateArguments{
                {"key", runtime::Value(runtime::KeyValue{row_key})},
                {"id", runtime::Value(*id)},
                {"label", runtime::Value(label != nullptr ? *label : *id)},
                {"value", runtime::Value(*id)},
                {"index", runtime::Value(static_cast<double>(index))},
                {"level", runtime::Value(0.0)},
                {"enabled", runtime::Value(row_enabled)},
                {"selected", runtime::Value(selected == index)},
                {"active", runtime::Value(active == index)},
                {"checked", runtime::Value(false)},
                {"separator", runtime::Value(false)},
                {"hasChildren", runtime::Value(false)},
                {"shortcut", runtime::Value("")},
            }
        );
        if (row != nullptr) rows.push_back(std::move(row));
    }
    std::shared_ptr<const DescriptionNode> popup = scope.instantiate_component(
        *popup_component,
        key + ".popup.surface",
        WidgetTemplateArguments{
            {"key", runtime::Value(runtime::KeyValue{key + ".popup.surface"})},
            {"level", runtime::Value(0.0)},
            {"expanded", runtime::Value(true)},
        }
    );
    if (popup == nullptr) return;
    popup = append_popup_children(popup, std::move(rows));
    DescriptionNode::Properties portal_properties = widget_transparent_properties();
    widget_mark_native_presentation(portal_properties);
    portal_properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(select_portal_layout(
            *popup,
            trigger_component != nullptr ? key + ".trigger" : key
        ))
    );
    description.children.push_back(scope.node(
        "Panel",
        key + ".popup",
        std::move(portal_properties),
        {std::move(popup)}
    ));
    scope.synthesized();
}

void radio_group_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("gap", runtime::Value(4.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
}

void combo_box_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("gap", runtime::Value(8.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
}

void radio_group_expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    if (!description.key.has_value()) {
        throw std::logic_error("validated RadioGroup must have a key");
    }
    description.children.clear();
    if (!description.properties.contains("onChange")) {
        description.properties.emplace("onChange", scope.action("RadioGroup"));
    }
    for (const runtime::Value* option : scope.list("options")) {
        const std::string* id = widget_description_string(option->field("id"));
        const std::string* label = widget_description_string(option->field("label"));
        if (id == nullptr || id->empty() || label == nullptr) continue;
        DescriptionNode::Properties item_properties;
        item_properties.emplace("semantics", runtime::ExpressionValue(widget_object({
            {"decorative", runtime::Value(true)},
        })));
        item_properties.emplace("$layout", runtime::ExpressionValue(widget_object({
            {"alignItems", runtime::Value("CENTER")},
            {"background", runtime::Value{}},
            {"border", runtime::Value{}},
            {"height", runtime::Value(28.0)},
            {"kind", runtime::Value("PANEL")},
            {"padding", widget_object({
                {"bottom", runtime::Value(5.0)},
                {"left", runtime::Value(28.0)},
                {"right", runtime::Value(8.0)},
                {"top", runtime::Value(5.0)},
            })},
            {"width", widget_fill()},
        })));
        description.children.push_back(scope.node(
            "Panel",
            *description.key + "." + *id,
            std::move(item_properties)
        ));
        scope.synthesized(1U);
    }
}

[[nodiscard]] std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void combo_box_expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    if (!description.key.has_value()) {
        throw std::logic_error("validated ComboBox must have a key");
    }
    const std::string query = scope.string("query");
    const std::string query_lower = lower(query);
    const bool enabled = scope.boolean("enabled", true);
    description.properties.insert_or_assign("$layout", runtime::ExpressionValue(widget_object({
        {"gap", widget_object({
            {"horizontal", runtime::Value(4.0)},
            {"vertical", runtime::Value(4.0)},
        })},
        {"height", runtime::Value("content")},
        {"kind", runtime::Value("COLUMN")},
        {"width", runtime::Value(200.0)},
    })));

    std::vector<runtime::Value> filtered;
    for (const runtime::Value* option : scope.list("options")) {
        const std::string* label = widget_description_string(option->field("label"));
        if (label != nullptr && (query_lower.empty() || lower(*label).contains(query_lower))) {
            filtered.push_back(*option);
        }
    }
    if (filtered.empty()) {
        filtered.push_back(widget_object({
            {"enabled", runtime::Value(false)},
            {"id", runtime::Value("__empty")},
            {"label", runtime::Value("No matches")},
        }));
    }

    DescriptionNode::Properties query_properties{
        {"enabled", runtime::ExpressionValue(runtime::Value(enabled))},
        {"hint", runtime::ExpressionValue(runtime::Value("Search..."))},
        {"text", runtime::ExpressionValue(runtime::Value(query))},
    };
    std::shared_ptr<const runtime::ActionValue> query_action = scope.bound_action("onQuery");
    query_properties.emplace(
        "onChange",
        query_action != nullptr
            ? runtime::ExpressionValue(std::move(query_action))
            : scope.action("ComboBoxQuery")
    );
    scope.apply_layout_defaults("TextBox", query_properties);

    std::string selected = scope.string("selectedId");
    const auto selected_entry = std::ranges::find_if(filtered, [&selected](const runtime::Value& option) {
        const std::string* id = widget_description_string(option.field("id"));
        return id != nullptr && *id == selected;
    });
    if (selected.empty() || selected_entry == filtered.end()) {
        const auto first_enabled = std::ranges::find_if(filtered, [](const runtime::Value& option) {
            const runtime::Value* value = option.field("enabled");
            return value == nullptr || value->boolean() == nullptr || *value->boolean();
        });
        const runtime::Value& first = first_enabled != filtered.end() ? *first_enabled : filtered.front();
        if (const std::string* id = widget_description_string(first.field("id")); id != nullptr) selected = *id;
    }
    std::string selected_label = selected;
    for (const runtime::Value& option : filtered) {
        const std::string* id = widget_description_string(option.field("id"));
        const std::string* label = widget_description_string(option.field("label"));
        if (id != nullptr && *id == selected && label != nullptr) selected_label = *label;
    }

    DescriptionNode::Properties select_properties{
        {"defaultSelectedId", runtime::ExpressionValue(runtime::Value(selected))},
        {"enabled", runtime::ExpressionValue(runtime::Value(enabled))},
        {"label", runtime::ExpressionValue(runtime::Value(selected_label))},
        {"options", runtime::ExpressionValue(runtime::Value(std::move(filtered)))},
    };
    std::shared_ptr<const runtime::ActionValue> select_action = scope.bound_action("onChange");
    select_properties.emplace(
        "onChange",
        select_action != nullptr
            ? runtime::ExpressionValue(std::move(select_action))
            : scope.action("ComboBoxSelect")
    );
    if (const runtime::Value* max_rows = scope.property("maxVisibleRows"); max_rows != nullptr) {
        select_properties.emplace("maxVisibleRows", runtime::ExpressionValue(*max_rows));
    }
    for (const std::string_view property : {
             "triggerTemplate",
             "popupTemplate",
             "itemTemplate",
         }) {
        if (const runtime::Value* value = scope.property(property); value != nullptr) {
            select_properties.emplace(
                std::string(property),
                runtime::ExpressionValue(*value)
            );
        }
    }
    scope.apply_layout_defaults("Select", select_properties);
    if (scope.property("triggerTemplate") == nullptr) {
        scope.set_layout(select_properties, "height", runtime::Value(32.0));
    }

    description.children = {
        scope.node("TextBox", *description.key + ".query", std::move(query_properties)),
        scope.node("Select", *description.key + ".select", std::move(select_properties)),
    };
    scope.synthesized(2U);
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const WidgetLayoutDefaultsHook defaults = nullptr,
    const WidgetDescriptionHook expand = nullptr,
    std::string default_action = {},
    std::string authored_presentation_property = {}
) {
    WidgetDescribePhase phase{
        defaults, expand, {}, std::move(default_action), true
    };
    phase.authored_presentation_property = std::move(authored_presentation_property);
    registry.register_describe_phase(
        std::move(type),
        std::move(phase)
    );
}

} // namespace

void register_control_widget_descriptions(WidgetRegistry& registry) {
    add(
        registry,
        "IconButton",
        &icon_button_defaults,
        &icon_button_expand,
        {},
        "presentationTemplate"
    );
    add(
        registry,
        "Checkbox",
        &checkbox_defaults,
        &checkbox_expand,
        {},
        "presentationTemplate"
    );
    add(
        registry,
        "Toggle",
        &toggle_defaults,
        &toggle_expand,
        {},
        "presentationTemplate"
    );
    add(
        registry,
        "Slider",
        &slider_defaults,
        &slider_expand,
        {},
        "presentationTemplate"
    );
    add(registry, "TextBox", &text_box_defaults);
    add(registry, "TextArea", &text_area_defaults, nullptr, "text-edit");
    add(registry, "NumberField", &number_field_defaults);
    add(
        registry,
        "Progress",
        &progress_defaults,
        &progress_expand,
        {},
        "presentationTemplate"
    );
    add(registry, "Tabs", &tabs_defaults);
    add(registry, "Select", &select_defaults, &select_expand);
    add(registry, "RadioGroup", &radio_group_defaults, &radio_group_expand, "RadioGroup");
    add(registry, "ComboBox", &combo_box_defaults, &combo_box_expand);

    WidgetCommandPhase icon_button_command;
    icon_button_command.references_property = "command";
    icon_button_command.activation_reference_property = "command";
    registry.register_command_phase("IconButton", std::move(icon_button_command));
}

} // namespace strata::ui
