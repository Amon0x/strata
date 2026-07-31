#include "ui/widget/description_collection_common.hpp"

#include <algorithm>
#include <cmath>

namespace strata::ui::collection_description {
namespace {

[[nodiscard]] bool header_kind(const runtime::Value* entry) noexcept {
    const std::string* kind = entry != nullptr
        ? widget_description_string(entry->field("kind"))
        : nullptr;
    return kind != nullptr && (*kind == "HEADER" || *kind == "GROUP");
}

void defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("clip", runtime::Value(true));
    scope.set("kind", runtime::Value("SCROLL"));
    scope.set("scrollHorizontal", runtime::Value(true));
    scope.set("scrollVertical", runtime::Value(true));
    scope.set("virtualItemExtent", runtime::Value(94.0));
    scope.set("virtualOverscan", runtime::Value(2.0));
}

void expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    description.children.clear();
    if (std::ranges::none_of(description.behaviors, [](const DescriptionBehavior& behavior) {
            return behavior.id == "strata.collection-marquee";
        })) {
        description.behaviors.push_back(DescriptionBehavior{
            "strata.collection-marquee", true, widget_object({}), nullptr,
        });
    }
    const std::size_t column_count = static_cast<std::size_t>(std::max(
        1.0,
        std::floor(scope.number("columns", 4.0))
    ));
    const double cell_width = scope.number("cellWidth", 112.0);
    const double cell_height = scope.number("cellHeight", 88.0);
    const double gap = scope.number("gap", 6.0);
    const std::string item_template = scope.string("itemTemplate");
    const double band_width = static_cast<double>(column_count) * cell_width +
        static_cast<double>(column_count - 1U) * gap;
    const std::set<std::string, std::less<>> selected = effective_keys(
        scope, "selectedKeys", "strata.collection.selected", "defaultSelectedKeys"
    );
    struct Band final {
        bool header = false;
        std::string key;
        std::vector<runtime::Value> entries;
    };
    const std::vector<const runtime::Value*> entries = scope.list("entries");
    std::vector<Band> bands;
    std::vector<runtime::Value> pending;
    const auto flush = [&bands, &pending, column_count]() {
        for (std::size_t start = 0U; start < pending.size(); start += column_count) {
            const std::size_t end = std::min(pending.size(), start + column_count);
            std::vector<runtime::Value> values(
                pending.begin() + static_cast<std::ptrdiff_t>(start),
                pending.begin() + static_cast<std::ptrdiff_t>(end)
            );
            const std::string& first_key = *widget_description_string(values.front().field("key"));
            bands.push_back(Band{false, "grid.band." + first_key, std::move(values)});
        }
        pending.clear();
    };
    for (const runtime::Value* entry : entries) {
        const std::string* key = widget_description_string(entry->field("key"));
        if (key == nullptr || key->empty()) continue;
        if (header_kind(entry)) {
            flush();
            bands.push_back(Band{true, *key, {*entry}});
        } else {
            const std::string* label = widget_description_string(entry->field("label"));
            if (label != nullptr) pending.push_back(*entry);
        }
    }
    flush();
    scope.set_layout("clip", runtime::Value(true));
    scope.set_layout("scrollHorizontal", runtime::Value(true));
    scope.set_layout("scrollVertical", runtime::Value(true));
    scope.set_layout("virtualItemCount", runtime::Value(static_cast<double>(bands.size())));
    scope.set_layout("virtualItemExtent", runtime::Value(cell_height + gap));
    std::vector<runtime::Value> band_keys;
    std::vector<double> band_extents;
    VirtualItemMembers band_members;
    band_keys.reserve(bands.size());
    band_extents.reserve(bands.size());
    band_members.reserve(bands.size());
    for (const Band& band : bands) {
        band_keys.emplace_back(runtime::KeyValue{band.key});
        band_extents.push_back(band.header ? 26.0 + gap : cell_height + gap);
        std::vector<std::string> members;
        if (!band.header) {
            members.reserve(band.entries.size());
            for (const runtime::Value& entry : band.entries) {
                const std::string* key = widget_description_string(entry.field("key"));
                if (key != nullptr) members.push_back(*key);
            }
        }
        band_members.push_back(std::move(members));
    }
    scope.set_layout("virtualItemKeys", runtime::Value(std::move(band_keys)));
    scope.set_layout("virtualOverscan", runtime::Value(scope.number("overscan", 2.0)));

    const std::size_t band_count = bands.size();
    WidgetGeneratedVirtualization virtualization{
        .item_members = std::make_shared<const VirtualItemMembers>(std::move(band_members)),
        .item_extents = std::make_shared<const collection::VirtualItemExtents>(
            std::move(band_extents)
        ),
    };
    scope.set_generated_children(
        band_count,
        [bands = std::move(bands), selected, item_template, cell_width, cell_height, gap,
         band_width](
            WidgetDescriptionScope& item_scope,
            const std::size_t index
        ) {
        const Band& band = bands.at(index);
        if (band.header) {
            const runtime::Value& entry = band.entries.front();
            const std::string* label = widget_description_string(entry.field("label"));
            DescriptionNode::Properties properties = widget_text_properties(
                label != nullptr ? *label : band.key,
                widget_object({
                    {"height", runtime::Value(26.0)},
                    {"width", runtime::Value(band_width)},
                })
            );
            properties.insert_or_assign("semantics", runtime::ExpressionValue(widget_object({
                {"label", runtime::Value(label != nullptr ? *label : band.key)},
                {"role", runtime::Value("GROUP")},
            })));
            item_scope.synthesized();
            return item_scope.node("Text", band.key, std::move(properties));
        }
        std::vector<std::shared_ptr<const DescriptionNode>> items;
        for (const runtime::Value& item : band.entries) {
            const std::string* key = widget_description_string(item.field("key"));
            const std::string* label = widget_description_string(item.field("label"));
            const bool is_selected = selected.contains(*key);
            std::shared_ptr<const DescriptionNode> item_node = item_template.empty()
                ? nullptr : item_scope.instantiate_component(
                item_template,
                *key,
                WidgetTemplateArguments{
                    {"key", runtime::Value(runtime::KeyValue{*key})},
                    {"label", runtime::Value(*label)},
                    {"selected", runtime::Value(is_selected)},
                    {"value", item.field("value") != nullptr
                        ? *item.field("value") : runtime::Value{}},
                }
            );
            if (item_node == nullptr) {
                item_node = item_scope.node("Text", *key, widget_text_properties(*label));
                item_scope.synthesized();
            }
            item_node = with_layout_fields(item_node, {
                {"height", runtime::Value(cell_height)},
                {"width", runtime::Value(cell_width)},
            });
            item_node = with_layout_padding(item_node, {
                {"left", 8.0}, {"top", 8.0}, {"right", 8.0},
            });
            item_node = with_semantics(item_node, widget_object({
                {"label", runtime::Value(*label)},
                {"role", runtime::Value("GRID_CELL")},
                {"selected", runtime::Value(is_selected)},
            }));
            items.push_back(std::move(item_node));
        }
        DescriptionNode::Properties properties = widget_transparent_properties();
        properties.insert_or_assign("$layout", runtime::ExpressionValue(widget_object({
            {"gap", widget_object({{"horizontal", runtime::Value(gap)}})},
            {"height", runtime::Value(cell_height)},
            {"kind", runtime::Value("ROW")},
            {"width", runtime::Value(band_width)},
        })));
        item_scope.synthesized();
        return item_scope.node(
            "Panel",
            band.key,
            std::move(properties),
            std::move(items)
        );
    }, std::move(virtualization));
}

} // namespace

void register_item_grid_description(WidgetRegistry& registry) {
    WidgetDescribePhase phase;
    phase.layout_defaults = &defaults;
    phase.expand = &expand;
    phase.starts_unmaterialized = true;
    registry.register_describe_phase("ItemGrid", std::move(phase));
}

} // namespace strata::ui::collection_description
