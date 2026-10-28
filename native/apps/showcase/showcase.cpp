#include "showcase.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <utility>

namespace strata::desktop {
namespace {

using strata::host::ActionEvent;
using strata::host::ActionResult;
using strata::host::DragEvent;
using strata::host::DropPlacement;
using strata::host::Value;
namespace contract = strata::contracts::demo_surface;

[[nodiscard]] std::optional<double> scalar_number(const Value& value) noexcept {
    if (value.number() != nullptr)
        return *value.number();
    return value.integer() != nullptr ? std::optional<double>(static_cast<double>(*value.integer()))
                                      : std::nullopt;
}

[[nodiscard]] std::string placement_name(const DropPlacement placement) {
    switch (placement) {
    case DropPlacement::on:
        return "on";
    case DropPlacement::before:
        return "before";
    case DropPlacement::after:
        return "after";
    }
    return "on";
}

} // namespace

ShowcaseModel::ShowcaseModel(std::string instance_label)
    : instance_label_(std::move(instance_label)), host_message_(instance_label_) {
    initialize_tree();
    initialize_grid();
}

const strata::host::Revision& ShowcaseModel::demo_revision() const noexcept {
    return demo_revision_;
}
const strata::host::Revision& ShowcaseModel::tree_revision() const noexcept {
    return tree_revision_;
}
const strata::host::Revision& ShowcaseModel::table_revision() const noexcept {
    return table_revision_;
}
const strata::host::Revision& ShowcaseModel::grid_revision() const noexcept {
    return grid_revision_;
}

void ShowcaseModel::initialize_tree() {
    tree_items_.reserve(101U);
    tree_items_.push_back(TreeItem{
        "data.tree.root",
        "Workspace (100,000 assets)",
        std::nullopt,
        std::nullopt,
        true,
    });
    for (std::size_t group = 0U; group < 100U; ++group) {
        std::ostringstream label;
        label << "Folder " << std::setw(3) << std::setfill('0') << group << " (1,000)";
        tree_items_.push_back(TreeItem{
            "data.tree.folder." + std::to_string(group),
            label.str(),
            std::string("data.tree.root"),
            group,
            true,
        });
    }
}

void ShowcaseModel::initialize_grid() {
    grid_groups_.resize(15U);
    for (std::size_t group = 0U; group < grid_groups_.size(); ++group) {
        grid_groups_[group].reserve(100U);
        for (std::size_t item = 0U; item < 100U; ++item) {
            grid_groups_[group].push_back(group * 100U + item);
        }
    }
}

std::vector<contract::DataTreeItemsItem> ShowcaseModel::tree_items() const {
    std::vector<contract::DataTreeItemsItem> values;
    values.reserve(tree_items_.size());
    for (const TreeItem& item : tree_items_) {
        const bool children_loaded = !item.folder || !item.folder_group.has_value() ||
                                     loaded_tree_groups_.contains(*item.folder_group);
        values.push_back(contract::DataTreeItemsItem{
            .key = item.key,
            .label = item.label,
            .parent_key = item.parent,
            .may_have_children = item.folder,
            .children_loaded = children_loaded,
        });
    }
    return values;
}

Value ShowcaseModel::tree_snapshot() const {
    return contract::encode_data_tree_items(tree_items());
}

Value ShowcaseModel::table_snapshot() const {
    std::vector<contract::DataTableRowsItem> rows;
    rows.reserve(5'000U);
    for (std::size_t index = 0U; index < 5'000U; ++index) {
        std::ostringstream name;
        name << "Entity " << std::setw(4) << std::setfill('0') << index;
        rows.push_back(contract::DataTableRowsItem{
            .key = "data.table.row." + std::to_string(index),
            .cells = contract::DataTableRowsItemCells{
                .name = name.str(),
                .status = index % 3U == 0U ? "Active" : "Idle",
                .progress = static_cast<double>(index % 101U),
            },
        });
    }
    return contract::encode_data_table_rows(rows);
}

Value ShowcaseModel::grid_snapshot() const {
    std::vector<contract::DataGridEntriesItem> entries;
    entries.reserve(1'515U);
    for (std::size_t group = 0U; group < grid_groups_.size(); ++group) {
        entries.push_back(contract::DataGridEntriesItem{
            .kind = "HEADER",
            .key = "data.grid.header." + std::to_string(group),
            .label = "Group " + std::to_string(group + 1U),
        });
        for (const std::size_t item : grid_groups_[group]) {
            entries.push_back(contract::DataGridEntriesItem{
                .kind = "ITEM",
                .key = "data.grid.item." + std::to_string(item),
                .label = "Icon " + std::to_string(item),
            });
        }
    }
    return contract::encode_data_grid_entries(entries);
}

Value ShowcaseModel::demo_snapshot() const {
    std::vector<contract::DemoReorderItemsItem> reorder;
    reorder.reserve(reorder_items_.size());
    for (const std::string& id : reorder_items_) {
        std::string label = id;
        if (!label.empty()) {
            label.front() =
                static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
        }
        reorder.push_back(contract::DemoReorderItemsItem{
            .id = id,
            .key = reorder_key(id),
            .label = std::move(label),
        });
    }
    return contract::encode_demo(contract::Demo{
        .event_total = 0.0,
        .retained_event_count = 0.0,
        .events = {},
        .coalescing_result = "not run yet",
        .deterministic_result = "not run yet",
        .focus_contained = false,
        .inspector_pick_armed = false,
        .reorder_items = std::move(reorder),
        .controlled_split_ratio = controlled_split_ratio_,
        .host_value = host_value_,
        .host_message = host_message_,
        .data_activity = data_activity_,
        .form_valid = false,
        .form_dirty = false,
        .form_touched = false,
        .combo_query = combo_query_,
        .combo_selection = combo_selection_,
        .inspector_node_count = 0.0,
        .inspector_selected_key = "none",
        .inspector_selected_type = "none",
        .inspector_bounds = "none",
        .inspector_actions = "none",
        .inspector_handler_owners = "none",
        .inspector_motion = "none",
        .measured_nodes = 0.0,
        .reused_nodes = 0.0,
        .arranged_nodes = 0.0,
        .surface_visible = surface_visible_,
    });
}

void ShowcaseModel::changed_demo() {
    demo_revision_.changed();
}

void ShowcaseModel::data_activity(std::string value) {
    if (value == data_activity_)
        return;
    data_activity_ = std::move(value);
    changed_demo();
}

void ShowcaseModel::surface_visible(const bool visible) {
    if (surface_visible_ == visible) return;
    surface_visible_ = visible;
    changed_demo();
}

bool ShowcaseModel::load_tree_children(const std::string_view key) {
    const std::optional<std::size_t> group = indexed_key(key, "data.tree.folder.");
    if (!group.has_value() || *group >= 100U || !loaded_tree_groups_.insert(*group).second) {
        return false;
    }
    tree_items_.reserve(tree_items_.size() + 1'000U);
    const std::string parent = "data.tree.folder." + std::to_string(*group);
    for (std::size_t item = 0U; item < 1'000U; ++item) {
        const std::size_t index = *group * 1'000U + item;
        std::ostringstream label;
        label << "Asset " << std::setw(6) << std::setfill('0') << index;
        tree_items_.push_back(TreeItem{
            "data.tree.item." + std::to_string(index),
            label.str(),
            parent,
            std::nullopt,
            false,
        });
    }
    tree_revision_.changed();
    data_activity("load children " + parent);
    return true;
}

std::string ShowcaseModel::reorder_key(const std::string_view id) {
    return "manipulate.reorder." + std::string(id);
}

bool ShowcaseModel::handle_reorder(const DragEvent& event) {
    if (!event.dropped() || event.payload_type != "demo.reorder" ||
        event.payload.string() == nullptr ||
        event.target_key != std::optional<std::string>("manipulate.reorder.container")) {
        return false;
    }
    const std::string source_key = reorder_key(*event.payload.string());
    const bool changed = strata::host::reorder_before_after(
        reorder_items_, source_key, event.before_key, event.after_key,
        [](const std::string& id) { return reorder_key(id); });
    if (changed)
        data_activity("reordered " + *event.payload.string());
    return changed;
}

bool ShowcaseModel::tree_descends_from(std::string key, const std::string_view ancestor) const {
    std::set<std::string, std::less<>> visited;
    while (visited.insert(key).second) {
        if (key == ancestor)
            return true;
        const auto item = std::ranges::find(tree_items_, key, &TreeItem::key);
        if (item == tree_items_.end() || !item->parent.has_value())
            return false;
        key = *item->parent;
    }
    return false;
}

bool ShowcaseModel::move_tree_item(const std::string_view source_key,
                                   const std::string_view target_key,
                                   const DropPlacement placement) {
    auto source = std::ranges::find(tree_items_, source_key, &TreeItem::key);
    const auto target = std::ranges::find(tree_items_, target_key, &TreeItem::key);
    if (source == tree_items_.end() || target == tree_items_.end() || source == target ||
        source->key == "data.tree.root") {
        return false;
    }
    std::optional<std::string> next_parent;
    if (placement == DropPlacement::on) {
        if (!target->folder)
            return false;
        next_parent = target->key;
    } else {
        if (!target->parent.has_value())
            return false;
        next_parent = target->parent;
    }
    if (next_parent.has_value() && tree_descends_from(*next_parent, source->key))
        return false;

    TreeItem moving = *source;
    moving.parent = next_parent;
    tree_items_.erase(source);
    std::size_t insertion = tree_items_.size();
    if (placement == DropPlacement::on) {
        bool has_child = false;
        for (std::size_t index = 0U; index < tree_items_.size(); ++index) {
            if (tree_items_[index].parent == next_parent) {
                insertion = index + 1U;
                has_child = true;
            }
        }
        if (!has_child) {
            const auto parent = std::ranges::find(tree_items_, *next_parent, &TreeItem::key);
            if (parent == tree_items_.end())
                return false;
            insertion = static_cast<std::size_t>(parent - tree_items_.begin()) + 1U;
        }
    } else {
        const auto current_target = std::ranges::find(tree_items_, target_key, &TreeItem::key);
        if (current_target == tree_items_.end())
            return false;
        insertion = static_cast<std::size_t>(current_target - tree_items_.begin()) +
                    (placement == DropPlacement::after ? 1U : 0U);
    }
    tree_items_.insert(tree_items_.begin() + static_cast<std::ptrdiff_t>(insertion),
                       std::move(moving));
    tree_revision_.changed();
    return true;
}

bool ShowcaseModel::handle_tree_drop(const DragEvent& event) {
    if (!event.dropped() || event.payload_type != "strata.tree-row" ||
        event.payload.string() == nullptr || !event.target_key.has_value()) {
        return false;
    }
    const bool changed =
        move_tree_item(*event.payload.string(), *event.target_key, event.placement);
    if (changed) {
        data_activity("moved " + *event.payload.string() + " " + placement_name(event.placement) +
                      " " + *event.target_key);
    }
    return changed;
}

std::optional<ShowcaseModel::GridLocation>
ShowcaseModel::grid_location(const std::size_t item) const noexcept {
    for (std::size_t group = 0U; group < grid_groups_.size(); ++group) {
        const auto found = std::ranges::find(grid_groups_[group], item);
        if (found != grid_groups_[group].end()) {
            return GridLocation{
                group,
                static_cast<std::size_t>(found - grid_groups_[group].begin()),
            };
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> ShowcaseModel::indexed_key(const std::string_view key,
                                                      const std::string_view prefix) noexcept {
    if (!key.starts_with(prefix))
        return std::nullopt;
    std::size_t value = 0U;
    const char* const first = key.data() + prefix.size();
    const char* const last = key.data() + key.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last ? std::optional<std::size_t>(value)
                                                          : std::nullopt;
}

bool ShowcaseModel::handle_grid_drop(const ActionEvent& action, const DragEvent& event) {
    if (!event.dropped() || event.payload_type != "demo.data-grid-item" ||
        event.payload.string() == nullptr || !event.target_key.has_value() ||
        action.source_key != event.target_key ||
        (event.placement != DropPlacement::before && event.placement != DropPlacement::after)) {
        return false;
    }
    const std::optional<std::size_t> source =
        indexed_key(*event.payload.string(), "data.grid.item.");
    const std::optional<std::size_t> target = indexed_key(*event.target_key, "data.grid.item.");
    if (!source.has_value() || !target.has_value() || source == target)
        return false;
    const std::optional<GridLocation> source_location = grid_location(*source);
    if (!source_location.has_value() || !grid_location(*target).has_value())
        return false;
    grid_groups_[source_location->group].erase(grid_groups_[source_location->group].begin() +
                                               static_cast<std::ptrdiff_t>(source_location->index));
    const std::optional<GridLocation> target_location = grid_location(*target);
    if (!target_location.has_value())
        return false;
    const std::size_t insertion =
        target_location->index + (event.placement == DropPlacement::after ? 1U : 0U);
    grid_groups_[target_location->group].insert(grid_groups_[target_location->group].begin() +
                                                    static_cast<std::ptrdiff_t>(insertion),
                                                *source);
    grid_revision_.changed();
    data_activity("moved data.grid.item." + std::to_string(*source) + " " +
                  placement_name(event.placement) + " data.grid.item." + std::to_string(*target));
    return true;
}

std::string ShowcaseModel::collection_activity(const ActionEvent& event) {
    if (event.kind == "tree-children-requested" && event.value.string() != nullptr) {
        return "load children " + *event.value.string();
    }
    if (event.kind == "collection-selection-changed") {
        const Value* selected = event.value.find("selectedKeys");
        const std::size_t count =
            selected != nullptr && selected->array() != nullptr ? selected->array()->size() : 0U;
        return "selection " + std::to_string(count) + " item(s)";
    }
    if (const std::optional<std::string_view> key = event.value.optional_string("key");
        key.has_value()) {
        return event.kind + " " + std::string(*key);
    }
    return event.kind + " " + event.source_key.value_or("unkeyed");
}

ActionResult ShowcaseModel::handle(const ActionEvent& event) {
    if (event.id == contract::DemoDataCollectionAction::id) {
        if (event.kind == "tree-children-requested" && event.value.string() != nullptr) {
            static_cast<void>(load_tree_children(*event.value.string()));
            return ActionResult::handled;
        }
        if (const std::optional<DragEvent> drag = DragEvent::from(event); drag.has_value()) {
            if (drag->payload_type == "strata.tree-row") {
                static_cast<void>(handle_tree_drop(*drag));
            } else if (drag->payload_type == "demo.data-grid-item") {
                static_cast<void>(handle_grid_drop(event, *drag));
            }
        } else {
            data_activity(collection_activity(event));
        }
        return ActionResult::handled;
    }
    if (event.id == contract::DemoReorderAction::id) {
        if (const std::optional<DragEvent> drag = DragEvent::from(event); drag.has_value()) {
            static_cast<void>(handle_reorder(*drag));
        }
        return ActionResult::handled;
    }
    if (event.id == contract::DemoSplitControlledAction::id) {
        if (const std::optional<double> value = scalar_number(event.value); value.has_value()) {
            const double next = std::clamp(*value, 0.0, 1.0);
            if (next != controlled_split_ratio_) {
                controlled_split_ratio_ = next;
                changed_demo();
            }
        }
        return ActionResult::handled;
    }
    if (event.id == contract::DemoHostBumpAction::id) {
        ++host_generation_;
        host_value_ = "desktop-v" + std::to_string(host_generation_);
        changed_demo();
        return ActionResult::handled;
    }
    if (event.id == contract::DemoHostMessageAction::id) {
        const contract::DemoHostMessageAction action =
            contract::DemoHostMessageAction::decode(event);
        const double priority_value = action.priority;
        if (priority_value < static_cast<double>(std::numeric_limits<int>::min()) ||
            priority_value > static_cast<double>(std::numeric_limits<int>::max())) {
            throw std::out_of_range("demo.host.message priority is outside the supported range");
        }
        const int priority = static_cast<int>(priority_value);
        host_message_ = action.message + " (priority=" + std::to_string(priority) + ")";
        changed_demo();
        return ActionResult::handled;
    }
    if (event.id == contract::DemoComboQueryAction::id && event.value.string() != nullptr) {
        combo_query_ = *event.value.string();
        changed_demo();
        return ActionResult::handled;
    }
    if (event.id == contract::DemoComboSelectAction::id) {
        combo_selection_ = event.value.string() != nullptr
                               ? std::optional<std::string>(*event.value.string())
                               : std::nullopt;
        changed_demo();
        return ActionResult::handled;
    }
    // The remaining showcase actions are probes whose desktop implementation intentionally has no
    // domain mutation. They are still valid handled capabilities rather than stringly fallthrough.
    return ActionResult::handled;
}

} // namespace strata::desktop
