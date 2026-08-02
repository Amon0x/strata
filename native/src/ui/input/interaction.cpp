#include "ui/input.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/choice_model.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

void append(InputOperationResult& destination, InputOperationResult source) {
    destination.injected_events += source.injected_events;
    destination.processed_events += source.processed_events;
    destination.events.insert(
        destination.events.end(),
        std::make_move_iterator(source.events.begin()),
        std::make_move_iterator(source.events.end())
    );
    destination.action_outcomes.insert(
        destination.action_outcomes.end(),
        std::make_move_iterator(source.action_outcomes.begin()),
        std::make_move_iterator(source.action_outcomes.end())
    );
}

} // namespace

RetainedNode* InputRouter::active_modal() const {
    if (tree_ == nullptr) return nullptr;
    if (const std::vector<RetainedNode*>* palettes = tree_->find_type("CommandPalette");
        palettes != nullptr) {
        for (auto iterator = palettes->rbegin(); iterator != palettes->rend(); ++iterator) {
            const runtime::Value* open = scalar_property(**iterator, "open");
            if (open == nullptr || open->boolean() == nullptr) {
                open = (*iterator)->retained_value("strata.palette.open");
            }
            if ((open == nullptr || open->boolean() == nullptr)) {
                open = scalar_property(**iterator, "defaultOpen");
            }
            if (open != nullptr && open->boolean() != nullptr && *open->boolean()) {
                return *iterator;
            }
        }
    }
    const std::vector<RetainedNode*>* modals = tree_->find_type("Modal");
    if (modals == nullptr) return nullptr;
    for (auto iterator = modals->rbegin(); iterator != modals->rend(); ++iterator) {
        const runtime::Value* open = scalar_property(**iterator, "open");
        if (open != nullptr && open->boolean() != nullptr && *open->boolean()) return *iterator;
    }
    return nullptr;
}

bool InputRouter::descendant_of(
    const RetainedNode& node,
    const RetainedNode& ancestor
) noexcept {
    for (const RetainedNode* current = &node; current != nullptr; current = current->parent()) {
        if (current == &ancestor) return true;
    }
    return false;
}

const DescriptionBehavior* InputRouter::behavior(
    const RetainedNode& node,
    const std::string_view id
) noexcept {
    const auto found = std::ranges::find(node.description().behaviors, id, &DescriptionBehavior::id);
    return found != node.description().behaviors.end() && found->enabled ? &*found : nullptr;
}

void InputRouter::dismiss_transient_popups(
    const RetainedNode* const target,
    InputOperationResult& result,
    const bool include_modal,
    const bool restore_modal_focus
) {
    if (tree_ == nullptr || tree_->root() == nullptr ||
        dismissing_transient_popups_) {
        return;
    }
    dismissing_transient_popups_ = true;
    bool modal_dismissed = false;
    const auto visit = [this, target, include_modal, &result, &modal_dismissed](
                           auto&& self,
                           RetainedNode& node
                       ) -> void {
        if (node.description().type == "MenuBar" &&
            (target == nullptr || !descendant_of(*target, node))) {
            const runtime::Value* category = node.retained_value("$menuCategory");
            if (category != nullptr && category->string() != nullptr && !category->string()->empty()) {
                const bool changed = tree_->set_retained_value(
                    node.identity(), "$menuCategory", runtime::Value{}, DirtyReason::properties
                );
                if (changed && description_invalidator_) description_invalidator_();
            }
        }
        const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
        if (lifecycle != nullptr && !lifecycle->input.popup_retained.empty() &&
            (include_modal || node.description().type != "CommandPalette") &&
            (target == nullptr || !descendant_of(*target, node))) {
            const WidgetInputPhase& input = lifecycle->input;
            const auto value = [&node](const std::string& name) -> const runtime::Value* {
                if (name.empty()) return nullptr;
                const auto found = node.description().properties.find(name);
                return found != node.description().properties.end() ? found->second.value() : nullptr;
            };
            const runtime::Value* open = value(input.popup_controlled);
            if (open == nullptr || open->boolean() == nullptr) {
                open = node.retained_value(input.popup_retained);
            }
            if ((open == nullptr || open->boolean() == nullptr) && !input.popup_initial.empty()) {
                open = value(input.popup_initial);
            }
            if (open != nullptr && open->boolean() != nullptr && *open->boolean()) {
                bool changed = tree_->set_retained_value(
                    node.identity(),
                    input.popup_retained,
                    runtime::Value(false),
                    DirtyReason::properties
                );
                if (node.description().type == "Select") {
                    if (const std::optional<EffectiveChoice> selected =
                            effective_choice(node);
                        selected.has_value()) {
                        changed = tree_->set_retained_value(
                            node.identity(),
                            "$choiceIndex",
                            runtime::Value(static_cast<double>(selected->index)),
                            DirtyReason::properties
                        ) || changed;
                    }
                }
                if (changed && description_invalidator_) description_invalidator_();
                modal_dismissed = modal_dismissed ||
                    (changed && node.description().type == "CommandPalette");
                const std::shared_ptr<const runtime::ActionValue> action = activation_action(
                    node,
                    input.popup_dismiss_action_property
                );
                JsonValue event = object({
                    {"action", action != nullptr ? canonical_action(*action, node) : JsonValue{}},
                    {"source", source(node)},
                    {"type", JsonValue("dismiss-requested")},
                });
                emit(std::move(event), action, node, runtime::Value{}, result);
            }
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    visit(visit, *tree_->root());
    if (modal_dismissed && restore_modal_focus) sync_modal_focus(result);
    dismissing_transient_popups_ = false;
}

InputOperationResult InputRouter::click(const std::string_view key) {
    if (tree_ == nullptr || layout_ == nullptr) throw std::logic_error("input requires a completed surface frame");
    RetainedNode* target = tree_->find_key(key);
    if (target == nullptr) throw std::invalid_argument("input target key is not retained");
    const Point center = injection_point(key);
    InputOperationResult result;
    append(result, pointer(PointerInputEvent{center, PointerEventType::press, 0, 0}));
    append(result, pointer(PointerInputEvent{center, PointerEventType::release, 0, 0}));
    synchronize_authored_presentations();
    return result;
}

void InputRouter::activate(
    RetainedNode& requested_target,
    InputOperationResult& result,
    const PointerInputEvent* const pointer,
    RetainedNode* const pointer_target,
    const std::size_t click_count,
    std::optional<WidgetSubtarget> subtarget
) {
    RetainedNode* target = &requested_target;
    if (!node_input_enabled(*target)) return;
    if (RetainedNode* modal = active_modal(); modal != nullptr) {
        const bool detached_above_modal = subtarget.has_value() && subtarget->detached &&
            subtarget->owner_identity == target->identity() &&
            subtarget->z_index > detached_overlay_z_index(*modal);
        const RetainedNode* focused = focused_.has_value() ? tree_->find_identity(*focused_) : nullptr;
        if (focused == nullptr || !descendant_of(*focused, *modal)) {
            focus(*modal, "programmatic", result);
        }
        if (!detached_above_modal && !descendant_of(*target, *modal)) {
            hover_route(modal);
            return;
        }
    }
    dismiss_transient_popups(target, result);
    hover_route(target);
    RetainedNode* const focus_target = pointer != nullptr && pointer_target != nullptr
        ? pointer_focusable_ancestor(pointer_target)
        : focusable_ancestor(target);
    if (focus_target != nullptr) {
        focus(*focus_target, "pointer", result);
    }
    for (RetainedNode* current = target; current != nullptr; current = current->parent()) {
        // Keyboard activation belongs to the focused control. Ancestors that intentionally own a
        // descendant key (such as Form/Enter) do so through their key lifecycle instead.
        if (pointer == nullptr && current != target) break;
        const WidgetLifecycle* lifecycle = widgets_.find(current->description().type);
        if (lifecycle == nullptr || lifecycle->input.click == nullptr) continue;
        WidgetInputScope scope(
            *this,
            *current,
            result,
            {},
            pointer != nullptr ? pointer->modifiers : KeyModifiers{},
            pointer,
            pointer_target,
            click_count,
            subtarget
        );
        if (lifecycle->input.click(scope)) break;
    }
}

} // namespace strata::ui
