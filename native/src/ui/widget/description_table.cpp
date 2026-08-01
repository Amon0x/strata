#include "ui/widget/description_collection_common.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace strata::ui::collection_description {
namespace {

struct Column final {
    runtime::Value value;
    std::string id;
    runtime::Value width;
    bool pinned = false;
};

class TableRowSequence final : public runtime::IndexableSequence {
public:
    TableRowSequence(
        runtime::Value source,
        std::vector<std::size_t> source_indices,
        std::vector<std::string> keys
    ) : source_(std::move(source)),
        source_indices_(std::move(source_indices)),
        keys_(std::move(keys)) {
        generation_ = UINT64_C(14695981039346656037);
        for (const std::string& key : keys_) {
            for (const char character : key) {
                generation_ = (generation_ ^ static_cast<unsigned char>(character)) *
                    UINT64_C(1099511628211);
            }
            generation_ = (generation_ ^ UINT64_C(0xFF)) * UINT64_C(1099511628211);
        }
    }

    [[nodiscard]] std::uint64_t generation() const noexcept override { return generation_; }
    [[nodiscard]] std::size_t count() const noexcept override { return keys_.size(); }
    [[nodiscard]] std::string key_at(const std::size_t index) const override {
        return keys_.at(index);
    }
    [[nodiscard]] std::optional<std::size_t> index_of_key(
        const std::string_view key
    ) const override {
        const auto found = std::ranges::find(keys_, key);
        return found != keys_.end()
            ? std::optional<std::size_t>(static_cast<std::size_t>(found - keys_.begin()))
            : std::nullopt;
    }
    [[nodiscard]] const runtime::Value& item_at(const std::size_t index) const override {
        return source_.list()->values.at(source_indices_.at(index));
    }
    [[nodiscard]] std::size_t source_index_at(const std::size_t index) const override {
        return source_indices_.at(index);
    }
    [[nodiscard]] bool same_generation(
        const runtime::KeyedSequence& other
    ) const noexcept override {
        const auto* rows = dynamic_cast<const TableRowSequence*>(&other);
        return rows != nullptr && rows->keys_ == keys_;
    }

private:
    runtime::Value source_;
    std::vector<std::size_t> source_indices_;
    std::vector<std::string> keys_;
    std::uint64_t generation_ = 0U;
};

[[nodiscard]] const runtime::Value* effective_list(
    WidgetDescriptionScope& scope,
    const std::string_view controlled,
    const std::string_view retained,
    const std::string_view defaults
) noexcept {
    const runtime::Value* value = scope.property(controlled);
    if (value == nullptr || value->list() == nullptr) value = scope.retained(retained);
    if (value == nullptr || value->list() == nullptr) value = scope.property(defaults);
    return value != nullptr && value->list() != nullptr ? value : nullptr;
}

[[nodiscard]] std::map<std::string, double, std::less<>> width_overrides(
    WidgetDescriptionScope& scope
) {
    std::map<std::string, double, std::less<>> result;
    const runtime::Value* values = effective_list(
        scope,
        "columnWidths",
        "strata.table.columnWidths",
        "defaultColumnWidths"
    );
    if (values == nullptr) return result;
    for (const runtime::Value& value : values->list()->values) {
        const std::string* id = widget_description_string(value.field("id"));
        const double* width = value.field("width") != nullptr ? value.field("width")->number() : nullptr;
        if (id != nullptr && width != nullptr && std::isfinite(*width)) {
            result.insert_or_assign(*id, *width);
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> column_order(WidgetDescriptionScope& scope) {
    std::vector<std::string> result;
    const runtime::Value* values = effective_list(
        scope,
        "columnOrder",
        "strata.table.columnOrder",
        "defaultColumnOrder"
    );
    if (values == nullptr) return result;
    for (const runtime::Value& value : values->list()->values) {
        const std::string* id = widget_description_string(&value);
        if (id != nullptr && !std::ranges::contains(result, *id)) result.push_back(*id);
    }
    return result;
}

[[nodiscard]] std::vector<Column> columns(WidgetDescriptionScope& scope) {
    const std::map<std::string, double, std::less<>> overrides = width_overrides(scope);
    const std::vector<std::string> order = column_order(scope);
    std::vector<Column> result;
    for (const runtime::Value* column : scope.list("columns")) {
        const std::string* id = widget_description_string(column->field("id"));
        if (id == nullptr || id->empty() || !boolean(column->field("visible"), true)) continue;
        const std::string size = widget_description_string(column->field("size")) != nullptr
            ? *widget_description_string(column->field("size"))
            : "WEIGHT";
        const auto override = overrides.find(*id);
        runtime::Value width;
        if (override != overrides.end()) {
            width = runtime::Value(override->second);
        } else if (size == "FIXED" || size == "CONTENT_MIN") {
            const runtime::Value* authored = column->field("width");
            width = runtime::Value(
                authored != nullptr && authored->number() != nullptr ? *authored->number() : 120.0
            );
        } else {
            const runtime::Value* weight = column->field("weight");
            width = widget_object({
                {"weight", runtime::Value(
                    weight != nullptr && weight->number() != nullptr ? *weight->number() : 1.0
                )},
            });
        }
        result.push_back(Column{*column, *id, std::move(width), boolean(column->field("pinned"))});
    }
    if (!order.empty()) {
        std::ranges::stable_sort(result, [&order](const Column& left, const Column& right) {
            const auto left_position = std::ranges::find(order, left.id);
            const auto right_position = std::ranges::find(order, right.id);
            const std::size_t left_index = left_position != order.end()
                ? static_cast<std::size_t>(left_position - order.begin()) : order.size();
            const std::size_t right_index = right_position != order.end()
                ? static_cast<std::size_t>(right_position - order.begin()) : order.size();
            return left_index < right_index;
        });
    }
    return result;
}

[[nodiscard]] std::map<std::string, std::string, std::less<>> cell_templates(
    WidgetDescriptionScope& scope
) {
    std::map<std::string, std::string, std::less<>> result;
    const runtime::Value* values = scope.property("cellTemplates");
    if (values == nullptr || values->list() == nullptr) return result;
    for (const runtime::Value& entry : values->list()->values) {
        const std::string* column = widget_description_string(entry.field("columnId"));
        const std::string* component = widget_description_string(entry.field("template"));
        if (column != nullptr && component != nullptr) result.insert_or_assign(*column, *component);
    }
    return result;
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> default_cell(
    WidgetDescriptionScope& scope,
    const std::string& key,
    const runtime::Value* value
) {
    std::string content;
    if (value != nullptr && value->number() != nullptr) {
        content = std::to_string(*value->number());
        while (content.size() > 2U && content.ends_with('0')) content.pop_back();
        if (content.ends_with('.')) content.push_back('0');
    } else if (value != nullptr) {
        content = runtime::display_string(*value);
    }
    scope.synthesized();
    return scope.node("Text", key, widget_text_properties(std::move(content)));
}

[[nodiscard]] std::string cell_label(const runtime::Value* value) {
    if (value == nullptr) return {};
    if (value->number() == nullptr) return runtime::display_string(*value);
    std::string result = std::to_string(*value->number());
    while (result.size() > 2U && result.ends_with('0')) result.pop_back();
    if (result.ends_with('.')) result.push_back('0');
    return result;
}

void defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("clip", runtime::Value(true));
    scope.set("contentPadding", widget_object({{"top", runtime::Value(32.0)}}));
    scope.set("kind", runtime::Value("SCROLL"));
    scope.set("scrollHorizontal", runtime::Value(true));
    scope.set("scrollVertical", runtime::Value(true));
    scope.set("virtualItemExtent", runtime::Value(30.0));
    scope.set("virtualOverscan", runtime::Value(3.0));
}

void expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    description.children.clear();
    const double row_height = scope.number("rowHeight", 30.0);
    const double header_height = scope.number("headerHeight", 32.0);
    const runtime::Value* row_source = scope.property("rows");
    const std::string collection_key = description.key.value_or("$table");
    bool async_rows = false;
    const auto placeholder = [&scope, &description, &collection_key](
                                 std::string text,
                                 const std::string_view key
                             ) {
        if (text.empty()) return;
        description.children.push_back(scope.node(
            "Text", collection_key + "." + std::string(key),
            widget_text_properties(std::move(text))
        ));
        description.synthesized_nodes += 1U;
    };
    if (row_source != nullptr && row_source->object() != nullptr &&
        row_source->field("status") != nullptr) {
        async_rows = true;
        const std::string* status = widget_description_string(row_source->field("status"));
        if (status == nullptr || *status == "IDLE" || *status == "LOADING") {
            placeholder(scope.string("loadingText", "Loading…"), "$async-loading");
            return;
        }
        if (*status == "FAILED") {
            const runtime::Value* error = row_source->field("error");
            const std::string* message = error != nullptr
                ? widget_description_string(error->field("message")) : nullptr;
            placeholder(
                scope.string("errorText", message != nullptr ? *message : "Unable to load data"),
                "$async-error"
            );
            return;
        }
        row_source = row_source->field("value");
    }
    const runtime::ValueList* authored_rows =
        row_source != nullptr ? row_source->list() : nullptr;
    if ((authored_rows == nullptr || authored_rows->values.empty()) && async_rows) {
        placeholder(scope.string("emptyText"), "$async-empty");
        return;
    }
    const std::vector<Column> resolved_columns = columns(scope);
    const std::set<std::string, std::less<>> selected = effective_keys(
        scope, "selectedKeys", "strata.collection.selected", "defaultSelectedKeys"
    );
    const std::map<std::string, std::string, std::less<>> templates = cell_templates(scope);
    const std::string row_template = scope.string("rowTemplate");
    const std::string generic_template = scope.string("cellTemplate");
    scope.set_layout("clip", runtime::Value(true));
    scope.set_layout("contentPadding", widget_object({{"top", runtime::Value(header_height)}}));
    scope.set_layout("scrollHorizontal", runtime::Value(true));
    scope.set_layout("scrollVertical", runtime::Value(true));
    std::vector<std::size_t> row_indices;
    std::vector<std::string> row_keys;
    if (authored_rows != nullptr) {
        row_indices.reserve(authored_rows->values.size());
        row_keys.reserve(authored_rows->values.size());
        for (std::size_t index = 0U; index < authored_rows->values.size(); ++index) {
            const runtime::Value& row = authored_rows->values[index];
            const std::string* row_key = widget_description_string(row.field("key"));
            const runtime::Value* cells = row.field("cells");
            if (row_key == nullptr || row_key->empty() || cells == nullptr ||
                cells->object() == nullptr) {
                continue;
            }
            row_indices.push_back(index);
            row_keys.push_back(*row_key);
        }
    }
    const auto row_sequence = std::make_shared<const TableRowSequence>(
        row_source != nullptr ? *row_source : runtime::Value(std::vector<runtime::Value>{}),
        std::move(row_indices),
        std::move(row_keys)
    );
    scope.set_layout(
        "virtualItemCount", runtime::Value(static_cast<double>(row_sequence->count()))
    );
    scope.set_layout("virtualItemExtent", runtime::Value(row_height));
    scope.set_layout("virtualOverscan", runtime::Value(scope.number("overscan", 3.0)));
    const auto rows_property = description.properties.find("rows");
    const bool sorted_view = rows_property != description.properties.end() &&
        rows_property->second.collection() != nullptr &&
        *rows_property->second.collection() != nullptr &&
        (*rows_property->second.collection())->operation == "sortBy";
    scope.set_layout(
        "virtualAnchorPolicy",
        runtime::Value(sorted_view ? "RESET_ON_CHANGE" : "PRESERVE")
    );

    const std::size_t row_count = row_sequence->count();
    scope.set_generated_children(
        row_count,
        [row_sequence, resolved_columns, selected, templates, row_template, generic_template,
         row_height](WidgetDescriptionScope& item_scope, const std::size_t index) {
        const runtime::Value& row = row_sequence->item_at(index);
        const std::string& row_key = *widget_description_string(row.field("key"));
        const runtime::Value& cells = *row.field("cells");
        const bool is_selected = selected.contains(row_key);
        std::shared_ptr<const DescriptionNode> row_node = row_template.empty()
            ? nullptr : item_scope.instantiate_component(
            row_template,
            row_key,
            WidgetTemplateArguments{
                {"cells", cells},
                {"key", runtime::Value(runtime::KeyValue{row_key})},
                {"selected", runtime::Value(is_selected)},
            }
        );
        if (row_node == nullptr) {
            std::vector<std::shared_ptr<const DescriptionNode>> cell_nodes;
            for (const Column& column : resolved_columns) {
                const runtime::Value* cell_value = cells.field(column.id);
                const std::string cell_key = row_key + ".cell." + column.id;
                std::string component = generic_template;
                if (const auto found = templates.find(column.id); found != templates.end()) {
                    component = found->second;
                }
                std::shared_ptr<const DescriptionNode> cell = component.empty()
                    ? nullptr
                    : item_scope.instantiate_component(
                          component,
                          cell_key,
                          WidgetTemplateArguments{
                              {"cells", cells},
                              {"columnId", runtime::Value(column.id)},
                              {"header", runtime::Value(
                                  widget_description_string(column.value.field("header")) != nullptr
                                      ? *widget_description_string(column.value.field("header"))
                                      : column.id
                              )},
                              {"key", runtime::Value(runtime::KeyValue{cell_key})},
                              {"rowKey", runtime::Value(runtime::KeyValue{row_key})},
                              {"selected", runtime::Value(is_selected)},
                              {"value", cell_value != nullptr ? *cell_value : runtime::Value{}},
                          }
                      );
                if (cell == nullptr) cell = default_cell(item_scope, cell_key, cell_value);
                cell = with_layout_fields(cell, {{"width", column.width}});
                cell = with_layout_padding(cell, {{"left", 8.0}, {"right", 8.0}});
                cell = with_semantics(cell, widget_object({
                    {"label", runtime::Value(cell_label(cell_value))},
                    {"role", runtime::Value("GRID_CELL")},
                    {"selected", runtime::Value(is_selected)},
                }));
                if (column.pinned) {
                    DescriptionNode::Properties pinned_properties{
                        {"$layout", runtime::ExpressionValue(widget_object({
                            {"alignItems", runtime::Value("CENTER")},
                            {"height", widget_fill()},
                            {"kind", runtime::Value("OVERLAY")},
                            {"width", column.width},
                            {"zIndex", runtime::Value(1.0)},
                        }))},
                        {"background", runtime::ExpressionValue(runtime::Value(
                            runtime::ColorValue{25U, 32U, 43U, 255U}
                        ))},
                        {"border", runtime::ExpressionValue(runtime::Value{})},
                        {"radius", runtime::ExpressionValue(runtime::Value(0.0))},
                        {"scrollPin", runtime::ExpressionValue(widget_object({
                            {"horizontal", runtime::Value(true)},
                        }))},
                    };
                    cell = item_scope.node(
                        "Panel",
                        row_key + ".pinned." + column.id,
                        std::move(pinned_properties),
                        {std::move(cell)}
                    );
                    item_scope.synthesized();
                }
                cell_nodes.push_back(std::move(cell));
            }
            DescriptionNode::Properties row_properties = widget_transparent_properties();
            row_properties.insert_or_assign("$layout", runtime::ExpressionValue(widget_object({
                {"alignItems", runtime::Value("CENTER")},
                {"height", runtime::Value(row_height)},
                {"kind", runtime::Value("ROW")},
                {"width", widget_fill()},
            })));
            row_node = item_scope.node(
                "Panel",
                row_key,
                std::move(row_properties),
                std::move(cell_nodes)
            );
            item_scope.synthesized();
        } else {
            row_node = with_layout_fields(row_node, {
                {"height", runtime::Value(row_height)},
                {"width", widget_fill()},
            });
        }
        row_node = with_semantics(row_node, widget_object({
            {"role", runtime::Value("ROW")},
            {"selected", runtime::Value(is_selected)},
        }));
        return row_node;
    }, WidgetGeneratedVirtualization{.sequence = row_sequence});
}

} // namespace

void register_table_description(WidgetRegistry& registry) {
    WidgetDescribePhase phase;
    phase.layout_defaults = &defaults;
    phase.expand = &expand;
    phase.starts_unmaterialized = true;
    registry.register_describe_phase("Table", std::move(phase));
}

} // namespace strata::ui::collection_description
