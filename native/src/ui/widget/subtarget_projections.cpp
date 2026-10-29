#include "ui/widget/subtarget.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>

#include "ui/command.hpp"
#include "ui/notification.hpp"
#include "ui/text_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/calendar_model.hpp"
#include "ui/widget/choice_model.hpp"
#include "ui/widget/menu_model.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/shell_model.hpp"

// Each built-in widget's presenter-owned pointer regions. Every projection here is registered as
// that widget's `inspection.subtargets` hook, the same path extension widgets use; input routing,
// inspection and presentation never ask for a widget by name.

namespace strata::ui {
namespace {

using Targets = std::vector<WidgetSubtarget>;

[[nodiscard]] const runtime::Value* property(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.data_value() : nullptr;
}

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

[[nodiscard]] double number(const runtime::Value* value, const double fallback) noexcept {
    return value != nullptr && value->number() != nullptr ? *value->number() : fallback;
}

[[nodiscard]] bool boolean(const runtime::Value* value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] std::string menu_bar_category(const CommandSnapshot& command) {
    if (command.category.empty()) return "Commands";
    const std::size_t separator = command.category.find('/');
    const std::string root = command.category.substr(0U, separator);
    return root.empty() ? "Commands" : root;
}

[[nodiscard]] bool effective_boolean(
    const RetainedNode& node,
    const std::string_view controlled,
    const std::string_view retained,
    const std::string_view initial,
    const bool fallback
) noexcept {
    const runtime::Value* value = property(node, controlled);
    if (value == nullptr || value->boolean() == nullptr) value = node.retained_value(retained);
    if (value == nullptr || value->boolean() == nullptr) value = property(node, initial);
    return boolean(value, fallback);
}

[[nodiscard]] Rect root_bounds(const LayoutResult& layout, const Rect fallback) noexcept {
    const LayoutRecord* root = layout.find(layout.root_identity);
    return root != nullptr ? root->bounds : fallback;
}

[[nodiscard]] const RetainedNode* descendant_key(
    const RetainedNode& node,
    const std::string_view key
) noexcept {
    if (node.description().key.has_value() && *node.description().key == key) return &node;
    for (const auto& child : node.children()) {
        if (const RetainedNode* found = descendant_key(*child, key); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<Rect> descendant_bounds(
    const RetainedNode& node,
    const std::string_view key,
    const LayoutResult& layout
) noexcept {
    const RetainedNode* child = descendant_key(node, key);
    const LayoutRecord* record = child != nullptr ? layout.find(child->identity()) : nullptr;
    return record != nullptr ? std::optional<Rect>(record->bounds) : std::nullopt;
}

[[nodiscard]] Rect popup_bounds(
    const Rect anchor,
    const Rect viewport,
    const double width,
    const double height
) noexcept {
    const double resolved_width = std::min(std::max(0.0, width), viewport.width);
    const double x = std::clamp(
        anchor.x,
        viewport.x,
        std::max(viewport.x, viewport.right() - resolved_width)
    );
    const double available_below = std::max(0.0, viewport.bottom() - anchor.bottom() - 2.0);
    const double available_above = std::max(0.0, anchor.y - viewport.y - 2.0);
    const bool place_above = height > available_below && available_above > available_below;
    const double resolved_height = std::min(
        std::max(0.0, height), place_above ? available_above : available_below
    );
    const double y = place_above
        ? anchor.y - 2.0 - resolved_height
        : anchor.bottom() + 2.0;
    return Rect{x, y, resolved_width, resolved_height};
}

void add_choice_rows(
    Targets& output,
    const RetainedNode& node,
    const runtime::ValueList& values,
    const Rect popup,
    const double row_height,
    const std::size_t maximum,
    const bool detached,
    const int z_index
) {
    const std::size_t count = std::min(values.values.size(), maximum);
    output.reserve(output.size() + count);
    for (std::size_t index = 0U; index < count; ++index) {
        const runtime::Value& entry = values.values[index];
        const std::string* id = text(entry.field("id"));
        if (id == nullptr || id->empty()) continue;
        const std::string* label = text(entry.field("label"));
        output.push_back(WidgetSubtarget{
            node.identity(),
            *id,
            WidgetSubtargetKind::choice,
            index,
            Rect{popup.x, popup.y + row_height * static_cast<double>(index), popup.width, row_height},
            runtime::Value(*id),
            label != nullptr ? *label : *id,
            {},
            boolean(entry.field("enabled"), true),
            detached,
            z_index,
        });
    }
}

[[nodiscard]] std::vector<const CommandSnapshot*> referenced(
    const RetainedNode& node,
    const CommandIndex* commands
) {
    return commands != nullptr ? commands->referenced_by(node)
                               : std::vector<const CommandSnapshot*>{};
}

Targets rich_text(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const LayoutRecord& record = context.record;
    const runtime::Value* combined = property(node, "text");
    if (combined == nullptr || combined->string() == nullptr || context.text_layout == nullptr ||
        !*context.text_layout) {
        return result;
    }
    const TextLayout shaped = (*context.text_layout)(
        node, *combined->string(), TextLayoutOptions{}
    );
    const Point origin{record.content_bounds.x, record.content_bounds.y};
    const std::vector<RichTextLink> links = rich_text_links(node);
    for (std::size_t link_index = 0U; link_index < links.size(); ++link_index) {
        const RichTextLink& link = links[link_index];
        for (const Rect& fragment : text_layout_selection_rects(
                 shaped, origin, link.utf16_start, link.utf16_end
             )) {
            const std::optional<Rect> clipped = fragment.intersection(record.content_bounds);
            if (!clipped.has_value()) continue;
            result.push_back(WidgetSubtarget{
                node.identity(), "$link/" + std::to_string(link.span_index),
                WidgetSubtargetKind::link, link_index, *clipped, runtime::Value{},
                combined->string()->substr(link.byte_start, link.byte_end - link.byte_start),
                {}, true, false, record.z_index,
            });
        }
    }
    return result;
}

Targets tabs(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const Rect bounds = context.record.bounds;
    const runtime::Value* source = property(node, "tabs");
    if (source == nullptr || source->list() == nullptr || source->list()->values.empty()) {
        return result;
    }
    const double width = bounds.width / static_cast<double>(source->list()->values.size());
    for (std::size_t index = 0U; index < source->list()->values.size(); ++index) {
        const runtime::Value& entry = source->list()->values[index];
        const std::string* id = text(entry.field("id"));
        if (id == nullptr || id->empty()) continue;
        const std::string* label = text(entry.field("label"));
        result.push_back(WidgetSubtarget{
            node.identity(), *id, WidgetSubtargetKind::choice, index,
            Rect{bounds.x + width * static_cast<double>(index), bounds.y, width, bounds.height},
            runtime::Value(*id), label != nullptr ? *label : *id, {},
            boolean(entry.field("enabled"), true), false, context.record.z_index,
        });
    }
    return result;
}

/** A Calendar's month buttons and its 42 day cells; a day's value is its "YYYY-MM-DD". */
Targets calendar(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const CalendarState state = calendar_state(node);
    const CalendarProjection projection = calendar_projection(state, context.record.bounds);
    const int z = context.record.z_index;
    result.push_back(WidgetSubtarget{node.identity(), "previous", WidgetSubtargetKind::control, 0U,
                                     projection.previous, runtime::Value("previous"),
                                     "Previous month", {}, true, false, z});
    result.push_back(WidgetSubtarget{node.identity(), "next", WidgetSubtargetKind::control, 1U,
                                     projection.next, runtime::Value("next"), "Next month", {},
                                     true, false, z});
    for (std::size_t index = 0U; index < projection.cells.size(); ++index) {
        const CalendarCell& cell = projection.cells[index];
        const std::string text = cell.date.text();
        result.push_back(WidgetSubtarget{
            node.identity(), "day:" + text, WidgetSubtargetKind::choice, index + 2U, cell.bounds,
            runtime::Value(text),
            state.month_name(cell.date.month) + " " + std::to_string(cell.date.day) + ", " +
                std::to_string(cell.date.year),
            {}, cell.enabled, false, z});
    }
    return result;
}

Targets radio_group(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const Rect bounds = context.record.bounds;
    const runtime::Value* source = property(node, "options");
    if (source == nullptr || source->list() == nullptr) return result;
    for (std::size_t index = 0U; index < source->list()->values.size(); ++index) {
        const runtime::Value& entry = source->list()->values[index];
        const std::string* id = text(entry.field("id"));
        if (id == nullptr || id->empty()) continue;
        const LayoutRecord* child = index < node.children().size()
            ? context.layout.find(node.children()[index]->identity())
            : nullptr;
        const double height = source->list()->values.empty()
            ? bounds.height
            : bounds.height / static_cast<double>(source->list()->values.size());
        const Rect row = child != nullptr
            ? child->bounds
            : Rect{bounds.x, bounds.y + height * static_cast<double>(index), bounds.width, height};
        const std::string* label = text(entry.field("label"));
        result.push_back(WidgetSubtarget{
            node.identity(), *id, WidgetSubtargetKind::choice, index, row,
            runtime::Value(*id), label != nullptr ? *label : *id, {},
            boolean(entry.field("enabled"), true), false, context.record.z_index,
        });
    }
    return result;
}

Targets select(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const LayoutResult& layout = context.layout;
    const Rect bounds = context.record.bounds;
    const std::string select_key = node.description().key.value_or("$select");
    const Rect control_bounds = property(node, "triggerTemplate") != nullptr
        ? descendant_bounds(node, select_key + ".trigger", layout).value_or(bounds)
        : bounds;
    result.push_back(WidgetSubtarget{
        node.identity(), "$control", WidgetSubtargetKind::control, 0U, control_bounds,
        runtime::Value{}, {}, {}, boolean(property(node, "enabled"), true), false,
        context.record.z_index,
    });
    const runtime::Value* source = property(node, "options");
    if (!effective_boolean(node, "expanded", "$expanded", "defaultExpanded", false) ||
        source == nullptr || source->list() == nullptr) return result;
    const bool authored_popup =
        property(node, "popupTemplate") != nullptr ||
        property(node, "itemTemplate") != nullptr;
    if (authored_popup) {
        const RetainedNode* popup_node = descendant_key(node, select_key + ".popup");
        const RetainedNode* popup_presentation = descendant_key(node, select_key + ".popup.surface");
        const LayoutRecord* popup_layout =
            popup_node != nullptr ? layout.find(popup_node->identity()) : nullptr;
        if (popup_layout != nullptr) {
            WidgetSubtarget surface{
                node.identity(), "$popup", WidgetSubtargetKind::separator, 0U,
                popup_layout->bounds, runtime::Value{}, {}, {}, false, true,
                detached_overlay_menu_z,
            };
            surface.separator = true;
            surface.presentation_identity = popup_presentation != nullptr
                ? popup_presentation->identity()
                : popup_node->identity();
            result.push_back(std::move(surface));
        }
        for (std::size_t index = 0U; index < source->list()->values.size(); ++index) {
            const runtime::Value& entry = source->list()->values[index];
            const std::string* id = text(entry.field("id"));
            if (id == nullptr || id->empty()) continue;
            const std::string* label = text(entry.field("label"));
            const RetainedNode* row_node = descendant_key(node, choice_option_key(select_key, *id));
            const LayoutRecord* row_layout =
                row_node != nullptr ? layout.find(row_node->identity()) : nullptr;
            if (row_layout == nullptr) continue;
            WidgetSubtarget row{
                node.identity(),
                *id,
                WidgetSubtargetKind::choice,
                index,
                row_layout->bounds,
                runtime::Value(*id),
                label != nullptr ? *label : *id,
                {},
                boolean(property(node, "enabled"), true) &&
                boolean(entry.field("enabled"), true),
                true,
                detached_overlay_menu_z,
            };
            row.presentation_identity = row_node->identity();
            result.push_back(std::move(row));
        }
        return result;
    }
    constexpr double row_height = 28.0;
    const std::size_t maximum = static_cast<std::size_t>(std::max(
        1.0, number(property(node, "maxVisibleRows"), 8.0)
    ));
    const std::size_t count = std::min(maximum, source->list()->values.size());
    const Rect popup = popup_bounds(
        bounds, root_bounds(layout, bounds), bounds.width,
        row_height * static_cast<double>(count)
    );
    add_choice_rows(
        result, node, *source->list(), popup, row_height, maximum, true,
        detached_overlay_menu_z
    );
    return result;
}

Targets menu(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const LayoutResult& layout = context.layout;
    const Rect bounds = context.record.bounds;
    const std::string menu_key = node.description().key.value_or("$menu");
    const Rect control_bounds = property(node, "triggerTemplate") != nullptr
        ? descendant_bounds(node, menu_key + ".trigger", layout).value_or(bounds)
        : bounds;
    result.push_back(WidgetSubtarget{
        node.identity(), "$control", WidgetSubtargetKind::control, 0U, control_bounds,
        runtime::Value{}, {}, {}, true, false, context.record.z_index,
    });
    if (!effective_boolean(node, "open", "$expanded", "defaultOpen", false)) return result;
    const MenuProjection projection = project_menu(node, layout, context.commands);
    const bool authored_popup =
        property(node, "popupTemplate") != nullptr ||
        property(node, "itemTemplate") != nullptr;
    if (authored_popup) {
        for (std::size_t level = 0U; level < projection.panels.size(); ++level) {
            const RetainedNode* popup_node =
                descendant_key(node, menu_key + ".popup." + std::to_string(level));
            const RetainedNode* popup_presentation =
                descendant_key(node, menu_key + ".popup.surface." + std::to_string(level));
            const LayoutRecord* popup_layout =
                popup_node != nullptr ? layout.find(popup_node->identity()) : nullptr;
            if (popup_layout == nullptr) continue;
            WidgetSubtarget surface{
                node.identity(),
                "$popup/" + std::to_string(level),
                WidgetSubtargetKind::separator,
                level,
                popup_layout->bounds,
                runtime::Value{},
                {},
                {},
                false,
                true,
                detached_overlay_menu_z,
            };
            surface.separator = true;
            surface.presentation_identity = popup_presentation != nullptr
                ? popup_presentation->identity()
                : popup_node->identity();
            result.push_back(std::move(surface));
        }
    }
    for (const MenuRowModel& row : projection.rows()) {
        if (row.item == nullptr) continue;
        const MenuItemModel& item = *row.item;
        const RetainedNode* row_node = authored_popup
            ? descendant_key(node, menu_row_key(menu_key, row.path))
            : nullptr;
        const LayoutRecord* row_layout =
            row_node != nullptr ? layout.find(row_node->identity()) : nullptr;
        if (authored_popup && row_layout == nullptr) continue;
        const Rect item_bounds = row_layout != nullptr ? row_layout->bounds : row.bounds;
        WidgetSubtarget target{
            node.identity(), menu_row_identity(row.path),
            item.separator ? WidgetSubtargetKind::separator
                : !item.command_id.empty() ? WidgetSubtargetKind::command
                : WidgetSubtargetKind::choice,
            row.path.back(), item_bounds, runtime::Value(item.id), item.label,
            item.command_id, item.enabled, true, detached_overlay_menu_z,
        };
        target.path = row.path;
        target.detail = item.shortcut;
        target.separator = item.separator;
        target.has_children = !item.children.empty();
        if (item.has_checked) target.checked = item.checked;
        if (row_node != nullptr) target.presentation_identity = row_node->identity();
        result.push_back(std::move(target));
    }
    return result;
}

Targets breadcrumbs(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const Rect bounds = context.record.bounds;
    const runtime::Value* source = property(node, "items");
    if (source == nullptr || source->list() == nullptr || source->list()->values.empty()) {
        return result;
    }
    const double width = bounds.width / static_cast<double>(source->list()->values.size());
    for (std::size_t index = 0U; index < source->list()->values.size(); ++index) {
        const runtime::Value& entry = source->list()->values[index];
        const std::string* id = text(entry.field("id"));
        if (id == nullptr) id = text(entry.field("key"));
        const std::string resolved = id != nullptr ? *id : std::to_string(index);
        const std::string* label = text(entry.field("label"));
        result.push_back(WidgetSubtarget{
            node.identity(), resolved, WidgetSubtargetKind::choice, index,
            Rect{bounds.x + width * static_cast<double>(index), bounds.y, width, bounds.height},
            runtime::Value(resolved), label != nullptr ? *label : resolved, {}, true, false,
            context.record.z_index,
        });
    }
    return result;
}

Targets list(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    for (std::size_t index = 0U; index < node.children().size(); ++index) {
        const RetainedNode& child = *node.children()[index];
        const LayoutRecord* child_layout = context.layout.find(child.identity());
        if (child_layout == nullptr || child_layout->bounds.empty()) continue;
        const std::string id = child.description().key.value_or(std::to_string(index));
        result.push_back(WidgetSubtarget{
            node.identity(), id, WidgetSubtargetKind::choice, index, child_layout->bounds,
            runtime::Value(runtime::KeyValue{id}), id, {}, true, false, context.record.z_index,
        });
    }
    return result;
}

Targets menu_bar(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const Rect bounds = context.record.bounds;
    const std::vector<const CommandSnapshot*> values = referenced(node, context.commands);
    std::vector<std::string> categories;
    for (const CommandSnapshot* command : values) {
        const std::string category = menu_bar_category(*command);
        if (std::ranges::find(categories, category) == categories.end()) {
            categories.push_back(category);
        }
    }
    if (categories.empty()) return result;
    const double width = std::min(112.0, bounds.width / static_cast<double>(categories.size()));
    for (std::size_t index = 0U; index < categories.size(); ++index) {
        result.push_back(WidgetSubtarget{
            node.identity(), "category:" + categories[index], WidgetSubtargetKind::control,
            index, Rect{bounds.x + width * static_cast<double>(index), bounds.y, width, bounds.height},
            runtime::Value(categories[index]), categories[index], {}, true, false,
            context.record.z_index,
        });
    }
    const std::string* open = text(node.retained_value("$menuCategory"));
    if (open == nullptr || open->empty()) return result;
    std::vector<const CommandSnapshot*> category_commands;
    for (const CommandSnapshot* command : values) {
        if (menu_bar_category(*command) == *open) category_commands.push_back(command);
    }
    const auto category = std::ranges::find(categories, *open);
    if (category == categories.end()) return result;
    const std::size_t category_index = static_cast<std::size_t>(category - categories.begin());
    const double row_height = std::max(18.0, number(property(node, "rowHeight"), 26.0));
    const Rect anchor{
        bounds.x + width * static_cast<double>(category_index), bounds.y, width, bounds.height,
    };
    const Rect popup = popup_bounds(
        anchor, root_bounds(context.layout, bounds), number(property(node, "menuWidth"), 200.0),
        row_height * static_cast<double>(category_commands.size())
    );
    for (std::size_t index = 0U; index < category_commands.size(); ++index) {
        const CommandSnapshot& command = *category_commands[index];
        result.push_back(WidgetSubtarget{
            node.identity(), "command:" + command.id, WidgetSubtargetKind::command, index,
            Rect{popup.x, popup.y + row_height * static_cast<double>(index), popup.width, row_height},
            runtime::Value(command.id), command.label, command.id, command.enabled, true,
            detached_overlay_menu_z,
        });
    }
    return result;
}

Targets toolbar(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const Rect bounds = context.record.bounds;
    const std::vector<const CommandSnapshot*> values = referenced(node, context.commands);
    const runtime::Value* fixed_width = property(node, "itemWidth");
    const bool measures = context.text_width != nullptr && *context.text_width;
    std::vector<double> widths;
    widths.reserve(values.size());
    for (const CommandSnapshot* command : values) {
        const double content_width = measures
            ? (*context.text_width)(node, command->label) + 24.0
            : 0.0;
        widths.push_back(std::max(
            36.0,
            fixed_width != nullptr && fixed_width->number() != nullptr
                ? *fixed_width->number()
                : content_width > 0.0 ? content_width : 96.0
        ));
    }
    const double overflow_width = std::max(
        36.0, number(property(node, "overflowWidth"), 48.0)
    );
    const double total_width = std::accumulate(widths.begin(), widths.end(), 0.0);
    const bool overflowed = total_width > bounds.width;
    const double command_limit = std::max(
        0.0, bounds.width - (overflowed ? overflow_width : 0.0)
    );
    std::size_t visible = 0U;
    double cursor = bounds.x;
    while (visible < values.size() && cursor - bounds.x + widths[visible] <= command_limit) {
        cursor += widths[visible++];
    }
    double command_x = bounds.x;
    for (std::size_t index = 0U; index < visible; ++index) {
        const CommandSnapshot& command = *values[index];
        result.push_back(WidgetSubtarget{
            node.identity(), "command:" + command.id, WidgetSubtargetKind::command, index,
            Rect{command_x, bounds.y, widths[index], bounds.height},
            runtime::Value(command.id), command.label, command.id, command.enabled, false,
            context.record.z_index,
        });
        command_x += widths[index];
    }
    if (!overflowed) return result;
    const double overflow_x = cursor;
    const double resolved_overflow_width = std::max(
        1.0, std::min(overflow_width, bounds.right() - overflow_x)
    );
    result.push_back(WidgetSubtarget{
        node.identity(), "$overflow", WidgetSubtargetKind::control, visible,
        Rect{overflow_x, bounds.y, resolved_overflow_width, bounds.height},
        runtime::Value(static_cast<double>(values.size() - visible)),
        "More commands", {}, true, false, context.record.z_index,
    });
    const runtime::Value* open = node.retained_value("$toolbarOverflow");
    if (open == nullptr || open->boolean() == nullptr || !*open->boolean()) return result;
    const double row_height = std::max(18.0, number(property(node, "rowHeight"), 28.0));
    const Rect popup = popup_bounds(
        result.back().bounds,
        root_bounds(context.layout, bounds),
        std::max(
            widths.empty() ? 0.0 : *std::max_element(widths.begin(), widths.end()),
            number(property(node, "menuWidth"), 220.0)
        ),
        row_height * static_cast<double>(values.size() - visible)
    );
    const std::size_t capacity = static_cast<std::size_t>(std::floor(popup.height / row_height));
    const std::size_t hidden_count = values.size() - visible;
    const runtime::Value* retained_offset = node.retained_value("$toolbarOverflowOffset");
    const std::size_t requested_offset = retained_offset != nullptr &&
        retained_offset->number() != nullptr && *retained_offset->number() >= 0.0
        ? static_cast<std::size_t>(*retained_offset->number())
        : 0U;
    const std::size_t offset = std::min(
        requested_offset, hidden_count > capacity ? hidden_count - capacity : 0U
    );
    const std::size_t end = std::min(hidden_count, offset + capacity);
    for (std::size_t hidden_index = offset; hidden_index < end; ++hidden_index) {
        const std::size_t index = visible + hidden_index;
        const CommandSnapshot& command = *values[index];
        result.push_back(WidgetSubtarget{
            node.identity(), "command:" + command.id, WidgetSubtargetKind::command, index,
            Rect{
                popup.x,
                popup.y + row_height * static_cast<double>(hidden_index - offset),
                popup.width,
                row_height,
            },
            runtime::Value(command.id), command.label, command.id, command.enabled,
            true, detached_overlay_menu_z,
        });
    }
    return result;
}

Targets banner(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const Rect bounds = context.record.bounds;
    const BannerProjection projection = project_banner(node);
    if (!projection.active) return result;
    const double dismiss_width = projection.dismissible ? std::min(30.0, bounds.width) : 0.0;
    if (projection.has_action) {
        const double action_width = std::min(120.0, std::max(0.0, bounds.width - dismiss_width));
        const std::string* label = text(property(node, "actionLabel"));
        result.push_back(WidgetSubtarget{
            node.identity(), std::string(banner_action_subtarget), WidgetSubtargetKind::action, 0U,
            Rect{bounds.right() - dismiss_width - action_width, bounds.y, action_width, bounds.height},
            runtime::Value{}, label != nullptr ? *label : std::string{}, {}, true, false,
            context.record.z_index,
        });
    }
    if (projection.dismissible) {
        result.push_back(WidgetSubtarget{
            node.identity(), std::string(banner_dismiss_subtarget), WidgetSubtargetKind::dismiss, 1U,
            Rect{bounds.right() - dismiss_width, bounds.y, dismiss_width, bounds.height},
            runtime::Value{}, "Dismiss", {}, true, false, context.record.z_index,
        });
    }
    return result;
}

Targets tooltip(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    if (property(node, "contentTemplate") != nullptr) {
        const std::string tooltip_key = node.description().key.value_or("$tooltip");
        if (const std::optional<Rect> popup =
                descendant_bounds(node, tooltip_key + ".content", context.layout);
            popup.has_value()) {
            result.push_back(WidgetSubtarget{
                node.identity(), "$tooltip", WidgetSubtargetKind::control, 0U,
                *popup, runtime::Value{}, {}, {}, true, true, detached_overlay_tooltip_z,
            });
        }
        return result;
    }
    static const WidgetTextLayoutResolver no_layout;
    const std::optional<TooltipProjection> projection = project_tooltip(
        node, context.layout, context.text_layout != nullptr ? *context.text_layout : no_layout
    );
    if (projection.has_value()) {
        result.push_back(WidgetSubtarget{
            node.identity(), "$tooltip", WidgetSubtargetKind::control, 0U,
            projection->bounds, runtime::Value{}, {}, {}, true, true,
            detached_overlay_tooltip_z,
        });
    }
    return result;
}

Targets modal(const WidgetSubtargetContext& context) {
    Targets result;
    if (!boolean(property(context.node, "dismissible"), true)) return result;
    result.push_back(WidgetSubtarget{
        context.node.identity(), "$scrim", WidgetSubtargetKind::scrim, 0U, context.record.bounds,
        runtime::Value{}, {}, {}, true, false, context.record.z_index,
    });
    return result;
}

Targets command_palette(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    if (!effective_boolean(node, "open", "strata.palette.open", "defaultOpen", false)) {
        return result;
    }
    const PaletteProjection projection = project_command_palette(
        node, context.layout, context.commands, context.edited_text
    );
    result.push_back(WidgetSubtarget{
        node.identity(), "$scrim", WidgetSubtargetKind::scrim, 0U,
        root_bounds(context.layout, context.record.bounds), runtime::Value{}, {}, {}, true, true,
        detached_overlay_palette_z,
    });
    result.push_back(WidgetSubtarget{
        node.identity(), "$panel", WidgetSubtargetKind::control, 0U,
        projection.bounds, runtime::Value{}, {}, {}, true, true,
        detached_overlay_palette_z + 1,
    });
    result.push_back(WidgetSubtarget{
        node.identity(), "$editor", WidgetSubtargetKind::control, 0U,
        projection.input_bounds, runtime::Value{}, {}, {}, true, true,
        detached_overlay_palette_z + 2,
    });
    for (std::size_t local = 0U; local < projection.visible_count(); ++local) {
        const std::size_t match_index = projection.window_start + local;
        const PaletteEntryModel& entry = projection.matches[match_index];
        WidgetSubtarget target{
            node.identity(), "$palette/" + entry.id,
            entry.command_id.empty() ? WidgetSubtargetKind::choice : WidgetSubtargetKind::command,
            match_index, projection.row_bounds(local), runtime::Value(entry.id),
            entry.label, entry.command_id, true, true,
            detached_overlay_palette_z + 3,
        };
        target.detail = entry.detail;
        target.path = {entry.source_index};
        result.push_back(std::move(target));
    }
    return result;
}

Targets toast_region(const WidgetSubtargetContext& context) {
    Targets result;
    if (context.notifications == nullptr) return result;
    const RetainedNode& node = context.node;
    static const WidgetTextLayoutResolver no_layout;
    const ToastProjection projection = project_toasts(
        node, context.layout, *context.notifications,
        context.text_layout != nullptr ? *context.text_layout : no_layout
    );
    for (std::size_t index = 0U; index < projection.cards.size(); ++index) {
        const ToastCardModel& card = projection.cards[index];
        const std::string prefix = "$toast/" + std::to_string(card.notification.id);
        WidgetSubtarget notification{
            node.identity(), prefix, WidgetSubtargetKind::notification, index, card.bounds,
            runtime::Value{}, card.notification.request.message, {}, true, true,
            detached_overlay_toast_z,
        };
        notification.notification_id = card.notification.id;
        result.push_back(std::move(notification));
        if (!card.action_bounds.empty()) {
            WidgetSubtarget action{
                node.identity(), prefix + "/action", WidgetSubtargetKind::action, index,
                card.action_bounds, runtime::Value{}, card.notification.request.action_label,
                {}, true, true, detached_overlay_toast_z + 1,
            };
            action.notification_id = card.notification.id;
            result.push_back(std::move(action));
        }
        WidgetSubtarget dismiss{
            node.identity(), prefix + "/dismiss", WidgetSubtargetKind::dismiss, index,
            card.dismiss_bounds, runtime::Value{}, "Dismiss", {}, true, true,
            detached_overlay_toast_z + 2,
        };
        dismiss.notification_id = card.notification.id;
        result.push_back(std::move(dismiss));
    }
    return result;
}

Targets chip_input(const WidgetSubtargetContext& context) {
    Targets result;
    const RetainedNode& node = context.node;
    const Rect bounds = context.record.bounds;
    const runtime::Value* source = property(node, "values");
    if (source == nullptr || source->list() == nullptr) source = node.retained_value("$values");
    if (source == nullptr || source->list() == nullptr) source = property(node, "defaultValues");
    const runtime::ValueList* values = source != nullptr ? source->list() : nullptr;
    const bool measures = context.text_width != nullptr && *context.text_width;
    std::vector<double> widths;
    if (values != nullptr) {
        widths.reserve(values->values.size());
        for (const runtime::Value& value : values->values) {
            const std::string* raw = text(&value);
            const std::string label = raw != nullptr ? *raw + " \xC3\x97" : std::string{};
            const double measured = measures && !label.empty()
                ? (*context.text_width)(node, label)
                : static_cast<double>(label.size()) * 7.0;
            widths.push_back(std::clamp(measured + 16.0, 38.0, 160.0));
        }
    }
    const double editor_reserve = 54.0;
    const double lane_width = std::max(1.0, bounds.width - 12.0 - editor_reserve);
    std::optional<std::size_t> active;
    if (const runtime::Value* value = node.retained_value("$activeToken");
        value != nullptr && value->number() != nullptr &&
        *value->number() >= 0.0 && *value->number() < static_cast<double>(widths.size())) {
        active = static_cast<std::size_t>(*value->number());
    }
    const std::size_t target = active.value_or(widths.empty() ? 0U : widths.size() - 1U);
    std::size_t start = widths.empty() ? 0U : target;
    double occupied = widths.empty() ? 0.0 : widths[target];
    while (start > 0U) {
        const double next = widths[start - 1U] + (occupied > 0.0 ? 5.0 : 0.0);
        if (occupied + next > lane_width) break;
        --start;
        occupied += next;
    }

    double cursor = bounds.x + 6.0;
    if (values != nullptr) {
        for (std::size_t index = start; index < widths.size(); ++index) {
            const std::string* label = text(&values->values[index]);
            if (label == nullptr) continue;
            const double width = std::min(widths[index], lane_width);
            if (index > start && cursor + width > bounds.x + 6.0 + lane_width) break;
            result.push_back(WidgetSubtarget{
                node.identity(), "token:" + std::to_string(index), WidgetSubtargetKind::token,
                index,
                Rect{cursor, bounds.y + 5.0, width, std::max(20.0, bounds.height - 10.0)},
                values->values[index], *label, {}, true, false, context.record.z_index,
            });
            cursor += width + 5.0;
        }
    }
    const double editor_right = std::max(bounds.x, bounds.right() - 6.0);
    cursor = std::min(cursor, editor_right);
    result.push_back(WidgetSubtarget{
        node.identity(), "$editor", WidgetSubtargetKind::control, widths.size(),
        Rect{cursor, bounds.y, std::max(0.0, editor_right - cursor), bounds.height},
        runtime::Value{}, {}, {}, true, false, context.record.z_index,
    });
    return result;
}

using Projection = Targets (*)(const WidgetSubtargetContext&);

/** A widget laid out at zero size projects nothing. */
template <Projection projection>
Targets within_bounds(const WidgetSubtargetContext& context) {
    return context.record.bounds.empty() ? Targets{} : projection(context);
}

} // namespace

void register_builtin_widget_subtargets(WidgetRegistry& registry) {
    registry.register_subtargets("RichText", &within_bounds<&rich_text>);
    registry.register_subtargets("Tabs", &within_bounds<&tabs>);
    registry.register_subtargets("Calendar", &within_bounds<&calendar>);
    registry.register_subtargets("RadioGroup", &within_bounds<&radio_group>);
    registry.register_subtargets("Select", &within_bounds<&select>, true);
    registry.register_subtargets("Menu", &within_bounds<&menu>, true);
    registry.register_subtargets("Breadcrumbs", &within_bounds<&breadcrumbs>);
    registry.register_subtargets("List", &within_bounds<&list>);
    registry.register_subtargets("MenuBar", &within_bounds<&menu_bar>, true);
    registry.register_subtargets("Toolbar", &within_bounds<&toolbar>, true);
    registry.register_subtargets("Banner", &within_bounds<&banner>);
    registry.register_subtargets("Tooltip", &within_bounds<&tooltip>, true);
    registry.register_subtargets("Modal", &within_bounds<&modal>);
    // The palette and toasts occupy the root plane, not their own (often empty) layout box.
    registry.register_subtargets("CommandPalette", &command_palette, true);
    registry.register_subtargets("ToastRegion", &toast_region, true);
    registry.register_subtargets("ChipInput", &within_bounds<&chip_input>);
}

} // namespace strata::ui
