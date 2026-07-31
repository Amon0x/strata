#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/expression.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"
#include "ui/presentation_geometry.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

struct FocusEntry final {
    RetainedNode* node = nullptr;
    std::optional<double> tab_index;
    std::size_t order = 0U;
};

void collect_focus_entries(
    RetainedNode& node,
    const auto& accepts,
    std::vector<FocusEntry>& result,
    std::size_t& order
) {
    if (accepts(node)) {
        const runtime::Value* authored = scalar_property(node, "tabIndex");
        result.push_back(FocusEntry{
            &node,
            authored != nullptr && authored->number() != nullptr &&
                    std::isfinite(*authored->number())
                ? std::optional(*authored->number())
                : std::nullopt,
            order,
        });
    }
    ++order;
    for (const auto& child : node.children()) {
        collect_focus_entries(*child, accepts, result, order);
    }
}

} // namespace

std::vector<RetainedNode*> InputRouter::focusable_nodes() const {
    std::vector<RetainedNode*> result;
    if (tree_ == nullptr || tree_->root() == nullptr) return result;
    RetainedNode* modal = active_modal();
    RetainedNode* boundary = focus_boundary();
    const bool missing_boundary = focus_containment_key_.has_value() && boundary == nullptr;
    std::vector<FocusEntry> entries;
    std::size_t order = 0U;
    collect_focus_entries(
        *tree_->root(),
        [this, modal, boundary, missing_boundary](const RetainedNode& node) {
            return !missing_boundary && tabbable(node) &&
                (modal == nullptr || descendant_of(node, *modal)) &&
                (boundary == nullptr || descendant_of(node, *boundary));
        },
        entries,
        order
    );
    std::ranges::stable_sort(entries, [](const FocusEntry& left, const FocusEntry& right) {
        const double left_index = left.tab_index.value_or(std::numeric_limits<double>::infinity());
        const double right_index = right.tab_index.value_or(std::numeric_limits<double>::infinity());
        return left_index != right_index ? left_index < right_index : left.order < right.order;
    });
    result.reserve(entries.size());
    for (const FocusEntry& entry : entries) result.push_back(entry.node);
    return result;
}

bool InputRouter::traverse_focus(
    const bool backwards,
    InputOperationResult& result
) {
    const std::vector<RetainedNode*> nodes = focusable_nodes();
    if (nodes.empty()) {
        const bool changed = focused_.has_value();
        clear_focus("traversal", result);
        return changed;
    }
    const auto current = focused_.has_value()
                             ? std::ranges::find(nodes, *focused_, &RetainedNode::identity)
                             : nodes.end();
    std::size_t index = 0U;
    if (current == nodes.end()) {
        index = backwards ? nodes.size() - 1U : 0U;
    } else {
        const std::size_t current_index = static_cast<std::size_t>(current - nodes.begin());
        index = backwards
                    ? (current_index == 0U ? nodes.size() - 1U : current_index - 1U)
                    : (current_index + 1U) % nodes.size();
    }
    focus(*nodes[index], "traversal", result);
    return true;
}

bool InputRouter::move_focus_spatial(
    const std::string_view direction,
    InputOperationResult& result
) {
    const std::vector<RetainedNode*> nodes = focusable_nodes();
    if (nodes.empty() || layout_ == nullptr) return false;
    RetainedNode* current = focused_.has_value() && tree_ != nullptr
                                ? tree_->find_identity(*focused_)
                                : nullptr;
    if (current == nullptr) {
        focus(*nodes.front(), "traversal", result);
        return true;
    }
    const auto presented = [this](const RetainedNode& node) -> std::optional<Rect> {
        const LayoutRecord* record = layout_->find(node.identity());
        if (record == nullptr) return std::nullopt;
        Rect bounds = hit_bounds_resolver_ ? hit_bounds_resolver_(node, *record)
                                           : record->hit_bounds;
        if (motion_ == nullptr) return bounds;
        MotionTransform transform;
        std::vector<const RetainedNode*> route;
        for (const RetainedNode* item = &node; item != nullptr; item = item->parent()) {
            route.push_back(item);
        }
        for (auto item = route.rbegin(); item != route.rend(); ++item) {
            transform = concatenate_presentation_transform(
                transform, local_presentation_transform(**item, *motion_)
            );
        }
        return transform_presentation_bounds(bounds, transform);
    };
    const std::optional<Rect> source = presented(*current);
    if (!source.has_value()) return false;

    struct Candidate final {
        RetainedNode* node = nullptr;
        bool in_beam = false;
        double score = 0.0;
        std::size_t order = 0U;
    };
    std::vector<Candidate> candidates;
    const bool horizontal = direction == "left" || direction == "right";
    const double source_x = source->x + source->width * 0.5;
    const double source_y = source->y + source->height * 0.5;
    for (std::size_t order = 0U; order < nodes.size(); ++order) {
        RetainedNode* node = nodes[order];
        if (node == current) continue;
        const std::optional<Rect> bounds = presented(*node);
        if (!bounds.has_value()) continue;
        const double dx = bounds->x + bounds->width * 0.5 - source_x;
        const double dy = bounds->y + bounds->height * 0.5 - source_y;
        const bool valid = direction == "left" ? dx < 0.0
                           : direction == "right" ? dx > 0.0
                           : direction == "up" ? dy < 0.0
                           : direction == "down" ? dy > 0.0
                           : false;
        if (!valid) continue;
        const double primary_gap = std::max(0.0, horizontal
            ? (direction == "right" ? bounds->x - source->right()
                                      : source->x - bounds->right())
            : (direction == "down" ? bounds->y - source->bottom()
                                     : source->y - bounds->bottom()));
        const double source_cross_start = horizontal ? source->y : source->x;
        const double source_cross_end = horizontal ? source->bottom() : source->right();
        const double target_cross_start = horizontal ? bounds->y : bounds->x;
        const double target_cross_end = horizontal ? bounds->bottom() : bounds->right();
        const double cross_gap = target_cross_end < source_cross_start
                                     ? source_cross_start - target_cross_end
                                 : target_cross_start > source_cross_end
                                     ? target_cross_start - source_cross_end
                                     : 0.0;
        candidates.push_back(Candidate{
            node,
            cross_gap == 0.0,
            primary_gap + cross_gap * 2.5,
            order,
        });
    }
    const bool has_beam = std::ranges::any_of(candidates, &Candidate::in_beam);
    const auto selected = std::ranges::min_element(candidates, [has_beam](
        const Candidate& left,
        const Candidate& right
    ) {
        if (has_beam && left.in_beam != right.in_beam) return left.in_beam;
        return left.score != right.score ? left.score < right.score : left.order < right.order;
    });
    if (selected == candidates.end() || (has_beam && !selected->in_beam)) return false;
    focus(*selected->node, "traversal", result);
    return true;
}

bool InputRouter::dismiss_topmost(InputOperationResult& result) {
    if (tree_ == nullptr || tree_->root() == nullptr) return false;
    std::vector<RetainedNode*> dismissible;
    const auto visit = [this, &dismissible](auto&& self, RetainedNode& node) -> void {
        const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
        if (lifecycle != nullptr && !lifecycle->input.popup_retained.empty()) {
            const WidgetInputPhase& input = lifecycle->input;
            const runtime::Value* open = scalar_property(node, input.popup_controlled);
            if (open == nullptr || open->boolean() == nullptr) open = node.retained_value(input.popup_retained);
            if ((open == nullptr || open->boolean() == nullptr) && !input.popup_initial.empty()) {
                open = scalar_property(node, input.popup_initial);
            }
            if (open != nullptr && open->boolean() != nullptr && *open->boolean()) {
                dismissible.push_back(&node);
            }
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    visit(visit, *tree_->root());

    RetainedNode* node = !dismissible.empty() ? dismissible.back() : active_modal();
    if (node == nullptr) return false;
    if (node->description().type == "Modal" &&
        !boolean_value(scalar_property(*node, "dismissible"), true)) {
        return false;
    }
    const WidgetLifecycle* lifecycle = widgets_.find(node->description().type);
    if (lifecycle == nullptr) return false;
    const WidgetInputPhase& input = lifecycle->input;
    if (!input.popup_retained.empty()) {
        const runtime::Value* controlled = scalar_property(*node, input.popup_controlled);
        if (controlled == nullptr || controlled->boolean() == nullptr) {
            static_cast<void>(tree_->set_retained_value(
                node->identity(), input.popup_retained, runtime::Value(false), DirtyReason::properties
            ));
        }
    }
    if (node->description().type == "CommandPalette") {
        static_cast<void>(tree_->set_retained_value(
            node->identity(), "$paletteActive", runtime::Value(0.0), DirtyReason::input
        ));
        static_cast<void>(clear_editor_text(*node, result));
    } else if (node->description().type == "Menu") {
        static_cast<void>(tree_->set_retained_value(
            node->identity(), "$menuPath", runtime::Value{}, DirtyReason::input
        ));
    }
    const std::shared_ptr<const runtime::ActionValue> action = activation_action(
        *node,
        !input.popup_dismiss_action_property.empty()
            ? std::string_view(input.popup_dismiss_action_property)
            : std::string_view("onDismiss")
    );
    JsonValue event = object({
        {"action", action != nullptr ? canonical_action(*action, *node) : JsonValue{}},
        {"source", source(*node)},
        {"type", JsonValue("dismiss-requested")},
    });
    emit(std::move(event), action, *node, runtime::Value{}, result);
    sync_modal_focus(result);
    return true;
}

void InputRouter::sync_modal_focus(InputOperationResult& result) {
    RetainedNode* next_modal = active_modal();
    const std::optional<std::uint64_t> next_identity = next_modal != nullptr
                                                          ? std::optional(next_modal->identity())
                                                          : std::nullopt;
    if (next_identity == synchronized_modal_) return;

    if (!next_identity.has_value()) {
        if (!synchronized_modal_.has_value()) return;
        const std::optional<std::uint64_t> restore = focus_before_modal_;
        synchronized_modal_.reset();
        focus_before_modal_.reset();
        if (restore.has_value() && tree_ != nullptr) {
            if (RetainedNode* node = tree_->find_identity(*restore); node != nullptr) {
                focus(*node, "programmatic", result);
                return;
            }
        }
        clear_focus("programmatic", result);
        return;
    }

    if (!synchronized_modal_.has_value()) focus_before_modal_ = focused_;
    synchronized_modal_ = next_identity;
    RetainedNode* current = focused_.has_value() && tree_ != nullptr
                                ? tree_->find_identity(*focused_)
                                : nullptr;
    if (current != nullptr && descendant_of(*current, *next_modal)) return;

    const std::vector<RetainedNode*> candidates = focusable_nodes();
    if (!candidates.empty()) {
        focus(*candidates.front(), "programmatic", result);
    } else {
        focus(*next_modal, "programmatic", result);
    }
}

void InputRouter::sanitize_focus(InputOperationResult& result) {
    if (focus_containment_key_.has_value() && focus_boundary() == nullptr) {
        focus_containment_key_.reset();
        focus_before_containment_.reset();
    }
    if (!focused_.has_value() || tree_ == nullptr) return;
    RetainedNode* node = tree_->find_identity(*focused_);
    RetainedNode* modal = active_modal();
    if (node == nullptr || !focusable(*node) ||
        (modal != nullptr && !descendant_of(*node, *modal)) ||
        !within_focus_containment(*node)) {
        clear_focus("invalid_target", result);
    }
}

} // namespace strata::ui
