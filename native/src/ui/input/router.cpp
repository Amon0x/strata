#include "ui/input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
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
#include "ui/behavior/registry.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/shell_model.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

constexpr std::string_view tooltip_pending_state = "strata.tooltip.pending";
constexpr std::string_view tooltip_deadline_state = "strata.tooltip.deadline";

[[nodiscard]] bool retained_boolean(
    const RetainedNode& node,
    const std::string_view name,
    const bool fallback
) noexcept {
    const runtime::Value* value = node.retained_value(name);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] std::optional<bool> retained_optional_boolean(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const runtime::Value* value = node.retained_value(name);
    return value != nullptr && value->boolean() != nullptr
        ? std::optional<bool>(*value->boolean())
        : std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> retained_deadline(
    const RetainedNode& node
) noexcept {
    const runtime::Value* value = node.retained_value(tooltip_deadline_state);
    return value != nullptr && value->duration() != nullptr
        ? std::optional<std::int64_t>(value->duration()->nanoseconds)
        : std::nullopt;
}

} // namespace

void InputProcessingConfig::validate() const {
    if (max_events_per_frame == 0U) {
        throw std::invalid_argument("input event frame budget must be positive");
    }
    if (max_dispatches_per_event == 0U) {
        throw std::invalid_argument("input dispatch budget must be positive");
    }
    if (max_queued_events < max_events_per_frame) {
        throw std::invalid_argument("input queue capacity must cover at least one frame");
    }
    if (!std::isfinite(scroll_step) || scroll_step <= 0.0) {
        throw std::invalid_argument("input scroll step must be finite and positive");
    }
    if (!std::isfinite(pointer_drag_slop) || pointer_drag_slop < 0.0) {
        throw std::invalid_argument("pointer drag slop must be finite and non-negative");
    }
    if (!std::isfinite(drag_auto_scroll_edge) || drag_auto_scroll_edge <= 0.0) {
        throw std::invalid_argument("drag auto-scroll edge must be finite and positive");
    }
    if (!std::isfinite(drag_auto_scroll_speed) || drag_auto_scroll_speed <= 0.0) {
        throw std::invalid_argument("drag auto-scroll speed must be finite and positive");
    }
    if (drag_auto_scroll_max_frame_nanos <= 0) {
        throw std::invalid_argument("drag auto-scroll frame clamp must be positive");
    }
    if (command_tooltip_delay_nanos <= 0) {
        throw std::invalid_argument("command tooltip delay must be positive");
    }
    if (multi_click_interval_nanos <= 0) {
        throw std::invalid_argument("multi-click interval must be positive");
    }
    if (!std::isfinite(multi_click_slop) || multi_click_slop < 0.0) {
        throw std::invalid_argument("multi-click slop must be finite and non-negative");
    }
}

InputRouter::InputRouter(
    std::string public_surface_id,
    std::string host_service_owner,
    runtime::ApplicationContext& application,
    const WidgetRegistry& widgets,
    const BehaviorRegistry& behaviors,
    StatusFeedbackService& status_feedback,
    NotificationService& notifications,
    SurfaceFrameworkExecutor surface_framework_executor,
    DescriptionInvalidator description_invalidator,
    HitBoundsResolver hit_bounds_resolver,
    TextOffsetResolver text_offset_resolver,
    TextWidthResolver text_width_resolver,
    ImeCursorRectResolver ime_cursor_rect_resolver,
    DragPreviewResolver drag_preview_resolver,
    InputProcessingConfig input_config,
    runtime::HostServices* const host_services,
    TextLayoutResolver text_layout_resolver,
    ScrollMutationObserver scroll_mutation_observer,
    FrameInvalidator frame_invalidator
) : public_surface_id_(std::move(public_surface_id)),
    host_service_owner_(host_service_owner.empty()
        ? public_surface_id_
        : std::move(host_service_owner)),
    application_(application),
    widgets_(widgets),
    behaviors_(behaviors),
    status_feedback_(status_feedback),
    notifications_(notifications),
    surface_framework_executor_(std::move(surface_framework_executor)),
    description_invalidator_(std::move(description_invalidator)),
    frame_invalidator_(std::move(frame_invalidator)),
    hit_bounds_resolver_(std::move(hit_bounds_resolver)),
    text_offset_resolver_(std::move(text_offset_resolver)),
    text_width_resolver_(std::move(text_width_resolver)),
    text_layout_resolver_(std::move(text_layout_resolver)),
    ime_cursor_rect_resolver_(std::move(ime_cursor_rect_resolver)),
    drag_preview_resolver_(std::move(drag_preview_resolver)),
    scroll_mutation_observer_(std::move(scroll_mutation_observer)),
    input_config_(std::move(input_config)),
    host_services_(host_services != nullptr ? host_services : &fallback_host_services_) {
    if (public_surface_id_.empty()) {
        throw std::invalid_argument("input router public surface id must not be empty");
    }
    if (host_service_owner_.empty()) {
        throw std::invalid_argument("input router host-service owner must not be empty");
    }
    input_config_.validate();
}

InputRouter::~InputRouter() {
    host_services_->request_ime(host_service_owner_, std::nullopt);
}

bool InputRouter::node_participates(const RetainedNode& node) const noexcept {
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    return lifecycle == nullptr || !lifecycle->participates || lifecycle->participates(node);
}

bool InputRouter::node_input_enabled(const RetainedNode& node) const noexcept {
    if (!node_participates(node)) return false;
    const runtime::Value* enabled = scalar_property(node, "enabled");
    if (enabled != nullptr && enabled->boolean() != nullptr && !*enabled->boolean()) return false;
    if (commands_ != nullptr) {
        const std::optional<CommandActivationBinding> binding =
            commands_->activation_binding(node);
        if (binding.has_value() && binding->command != nullptr && !binding->command->enabled) {
            return false;
        }
    }
    for (const DescriptionBehavior& attachment : node.description().behaviors) {
        if (!attachment.enabled) continue;
        const BehaviorLifecycle* behavior = behaviors_.find(attachment.id);
        if (behavior != nullptr && behavior->input.disabled) return false;
    }
    return motion_input_eligible(node);
}

bool InputRouter::command_tooltip_candidate(const std::uint64_t identity) const noexcept {
    if (tree_ == nullptr || commands_ == nullptr) return false;
    const RetainedNode* node = tree_->find_identity(identity);
    if (node == nullptr) return false;
    for (const RetainedNode* ancestor = node->parent(); ancestor != nullptr;
         ancestor = ancestor->parent()) {
        if (ancestor->description().type == "Tooltip") return false;
    }
    const std::optional<CommandActivationBinding> binding =
        commands_->activation_binding(*node);
    return binding.has_value() && binding->command != nullptr &&
           !binding->command->label.empty();
}

bool InputRouter::tooltip_engaged(const RetainedNode& node) const noexcept {
    if (hovered_.contains(node.identity())) return true;
    const RetainedNode* focused = focused_.has_value() && tree_ != nullptr
        ? tree_->find_identity(*focused_)
        : nullptr;
    return focused != nullptr && descendant_of(*focused, node);
}

bool InputRouter::tooltip_disclosures_need_frame() const noexcept {
    if (tree_ == nullptr) return false;
    const std::vector<RetainedNode*>* tooltips = tree_->find_type("Tooltip");
    if (tooltips == nullptr) return false;
    return std::ranges::any_of(*tooltips, [this](const RetainedNode* node) {
        if (node == nullptr || tooltip_controlled_visible(*node).has_value()) return false;
        const bool shown = retained_boolean(*node, tooltip_shown_state, false);
        const bool desired = tooltip_engaged(*node);
        return shown != desired || retained_optional_boolean(*node, tooltip_pending_state).has_value() ||
               retained_deadline(*node).has_value();
    });
}

void InputRouter::update_tooltip_disclosures() {
    if (tree_ == nullptr) return;
    const std::vector<RetainedNode*>* tooltips = tree_->find_type("Tooltip");
    if (tooltips == nullptr) return;
    bool changed = false;
    const auto set = [this, &changed](
                         RetainedNode& node,
                         std::string name,
                         runtime::Value value
                     ) {
        changed = tree_->set_retained_value(
            node.identity(), std::move(name), std::move(value), DirtyReason::input
        ) || changed;
    };
    for (RetainedNode* node : *tooltips) {
        if (node == nullptr) continue;
        if (tooltip_controlled_visible(*node).has_value()) {
            set(*node, std::string(tooltip_pending_state), runtime::Value{});
            set(*node, std::string(tooltip_deadline_state), runtime::Value{});
            continue;
        }
        const bool desired = tooltip_engaged(*node);
        const bool shown = retained_boolean(*node, tooltip_shown_state, false);
        std::optional<bool> pending = retained_optional_boolean(*node, tooltip_pending_state);
        std::optional<std::int64_t> deadline = retained_deadline(*node);
        if (desired == shown) {
            if (pending.has_value()) {
                set(*node, std::string(tooltip_pending_state), runtime::Value{});
            }
            if (deadline.has_value()) {
                set(*node, std::string(tooltip_deadline_state), runtime::Value{});
            }
            continue;
        }
        if (pending != desired || !deadline.has_value()) {
            const std::int64_t delay = desired
                ? tooltip_show_delay_nanos(*node)
                : tooltip_hide_delay_nanos(*node);
            const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
            const std::int64_t next_deadline =
                frame_time_nanos_ > maximum - delay
                ? maximum
                : frame_time_nanos_ + delay;
            set(*node, std::string(tooltip_pending_state), runtime::Value(desired));
            set(
                *node,
                std::string(tooltip_deadline_state),
                runtime::Value(runtime::DurationValue{next_deadline})
            );
            pending = desired;
            deadline = next_deadline;
        }
        if (pending == desired && deadline.has_value() && frame_time_nanos_ >= *deadline) {
            set(*node, std::string(tooltip_shown_state), runtime::Value(desired));
            set(*node, std::string(tooltip_pending_state), runtime::Value{});
            set(*node, std::string(tooltip_deadline_state), runtime::Value{});
        }
    }
    if (changed && frame_invalidator_) frame_invalidator_();
}

bool InputRouter::hover_ready(
    const std::uint64_t identity,
    const std::int64_t delay_nanos
) const noexcept {
    if (!hovered_.contains(identity)) return false;
    const auto started = hover_started_nanos_.find(identity);
    return started != hover_started_nanos_.end() && frame_time_nanos_ >= started->second &&
        frame_time_nanos_ - started->second >= std::max<std::int64_t>(0, delay_nanos);
}

bool InputRouter::focusable(const RetainedNode& node) const noexcept {
    if (!node_input_enabled(node)) return false;
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    bool focusable = lifecycle != nullptr && lifecycle->input.focusable;
    if (lifecycle != nullptr &&
        lifecycle->input.text_edit_mode == WidgetTextEditMode::static_text) {
        // Static focus follows the selection contract rather than the mere presence of a shared
        // lifecycle. RichText link subtargets remain independently focusable after opt-out.
        focusable = static_text_selectable(node) ||
            (node.description().type == "RichText" && !rich_text_links(node).empty());
    }
    if (focusable && !lifecycle->input.focusable_when.empty()) {
        const runtime::Value* condition = scalar_property(
            node,
            lifecycle->input.focusable_when
        );
        focusable = condition != nullptr && condition->boolean() != nullptr &&
                    *condition->boolean();
    }
    for (const DescriptionBehavior& attachment : node.description().behaviors) {
        if (!attachment.enabled) continue;
        const BehaviorLifecycle* behavior = behaviors_.find(attachment.id);
        if (behavior == nullptr) continue;
        focusable = focusable || behavior->input.focusable;
    }
    return focusable;
}

bool InputRouter::tabbable(const RetainedNode& node) const noexcept {
    if (!focusable(node)) return false;
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    if (lifecycle != nullptr &&
        lifecycle->input.text_edit_mode == WidgetTextEditMode::static_text) {
        // Frozen static text is pointer-focusable for selection, but only activatable RichText
        // links enter traversal. Text selection itself does not add a tab stop.
        return node.description().type == "RichText" && !rich_text_links(node).empty();
    }
    return lifecycle == nullptr || lifecycle->input.tabbable;
}

RetainedNode* InputRouter::focusable_ancestor(RetainedNode* node) const noexcept {
    for (RetainedNode* current = node; current != nullptr; current = current->parent()) {
        if (inside_native_presentation(*current)) continue;
        if (focusable(*current)) return current;
    }
    return nullptr;
}

bool InputRouter::passive_pointer_path(
    const RetainedNode& owner,
    const RetainedNode* pointer_target
) const noexcept {
    for (const RetainedNode* current = pointer_target;
         current != nullptr && current != &owner;
         current = current->parent()) {
        if (inside_native_presentation(*current)) continue;
        const WidgetLifecycle* lifecycle = widgets_.find(current->description().type);
        const bool static_text = lifecycle != nullptr &&
            lifecycle->input.text_edit_mode == WidgetTextEditMode::static_text;
        const bool linked_text = static_text && routed_subtarget_.has_value() &&
            routed_subtarget_->owner_identity == current->identity();
        if (linked_text || (lifecycle != nullptr && !static_text &&
            (lifecycle->input.event != nullptr || lifecycle->input.pointer != nullptr ||
             lifecycle->input.click != nullptr || focusable(*current)))) {
            return false;
        }
        for (const DescriptionBehavior& attachment : current->description().behaviors) {
            const BehaviorLifecycle* behavior = attachment.enabled
                ? behaviors_.find(attachment.id)
                : nullptr;
            if (behavior != nullptr && !behavior->input.disabled &&
                (behavior->input.accepts_pointer || behavior->input.focusable)) {
                return false;
            }
        }
    }
    return pointer_target != nullptr && descendant_of(*pointer_target, owner);
}

RetainedNode* InputRouter::pointer_focusable_ancestor(RetainedNode* node) const noexcept {
    RetainedNode* static_text_fallback = nullptr;
    for (RetainedNode* current = node; current != nullptr; current = current->parent()) {
        if (inside_native_presentation(*current)) continue;
        if (!focusable(*current)) continue;
        const WidgetLifecycle* lifecycle = widgets_.find(current->description().type);
        const bool static_text = lifecycle != nullptr &&
            lifecycle->input.text_edit_mode == WidgetTextEditMode::static_text;
        if (!static_text) return current;

        bool owns_interaction = routed_subtarget_.has_value() &&
            routed_subtarget_->owner_identity == current->identity();
        for (const DescriptionBehavior& attachment : current->description().behaviors) {
            const BehaviorLifecycle* behavior = attachment.enabled
                ? behaviors_.find(attachment.id)
                : nullptr;
            owns_interaction = owns_interaction || (behavior != nullptr &&
                !behavior->input.disabled &&
                (behavior->input.accepts_pointer || behavior->input.focusable));
        }
        if (owns_interaction) return current;
        if (static_text_fallback == nullptr) static_text_fallback = current;
    }
    return static_text_fallback;
}

void InputRouter::apply_pointer_focus_default(
    const PointerInputEvent& event,
    RetainedNode* const pointer_target,
    const RetainedNode* const interaction_target,
    InputOperationResult& result
) {
    const WidgetLifecycle* interaction = interaction_target != nullptr
        ? widgets_.find(interaction_target->description().type)
        : nullptr;
    if (interaction != nullptr &&
        interaction->input.pointer_focus == WidgetPointerFocusPolicy::preserve) {
        return;
    }
    if (RetainedNode* focus_target = pointer_focusable_ancestor(pointer_target);
        focus_target != nullptr) {
        focus(*focus_target, "pointer", result);
    } else if (event.button == 0 && !focus_contained()) {
        clear_focus("pointer", result);
    }
}

void InputRouter::begin_tree_update() {
    pending_focus_.reset();
    if (!focused_.has_value() || tree_ == nullptr) return;
    if (const RetainedNode* node = tree_->find_identity(*focused_); node != nullptr) {
        pending_focus_ = PendingFocus{node->identity(), source(*node)};
    }
}

InputOperationResult InputRouter::prepare(
    RetainedTree& tree,
    const std::optional<std::string_view> restore_focus_key
) {
    InputOperationResult result;
    tree_ = &tree;
    if (focused_.has_value() && tree.find_identity(*focused_) == nullptr) {
        if (pending_focus_.has_value() && pending_focus_->identity == *focused_) {
            JsonValue event = object({
                {"action", JsonValue{}},
                {"focused", JsonValue(false)},
                {"reason", JsonValue("invalid_target")},
                {"source", pending_focus_->source},
                {"type", JsonValue("focus-changed")},
            });
            result.action_outcomes.push_back(no_action_outcome(event));
            result.events.push_back(std::move(event));
            if (frame_invalidator_) frame_invalidator_();
        }
        focused_.reset();
    }
    pending_focus_.reset();
    const auto retain = [&tree](std::optional<std::uint64_t>& identity) {
        if (identity.has_value() && tree.find_identity(*identity) == nullptr) identity.reset();
    };
    retain(active_);
    if (hovered_subtarget_.has_value() &&
        tree.find_identity(hovered_subtarget_->first) == nullptr) {
        if (hovered_notification_id_.has_value()) {
            static_cast<void>(notifications_.pause(*hovered_notification_id_, false));
            hovered_notification_id_.reset();
        }
        hovered_subtarget_.reset();
    }
    if (active_subtarget_.has_value() &&
        tree.find_identity(active_subtarget_->first) == nullptr) {
        active_subtarget_.reset();
    }
    if (scrollbar_drag_.has_value() &&
        tree.find_identity(scrollbar_drag_->identity) == nullptr) {
        scrollbar_drag_.reset();
    }
    std::erase_if(pressed_pointer_targets_, [&tree](const auto& entry) {
        return tree.find_identity(entry.second.identity) == nullptr;
    });
    std::erase_if(drag_sessions_, [&tree](auto& entry) {
        DragSession& session = entry.second;
        if (tree.find_identity(session.source_identity) == nullptr) return true;
        if (session.target_identity.has_value() &&
            tree.find_identity(*session.target_identity) == nullptr) {
            session.target_identity.reset();
            session.operation.reset();
            session.placement = "on";
        }
        return false;
    });
    if (last_text_click_.has_value() && tree.find_identity(*last_text_click_) == nullptr) {
        last_text_click_.reset();
        text_click_count_ = 0U;
    }
    std::erase_if(hovered_, [&tree](const std::uint64_t identity) {
        return tree.find_identity(identity) == nullptr;
    });
    std::erase_if(hover_started_nanos_, [&tree](const auto& entry) {
        return tree.find_identity(entry.first) == nullptr;
    });
    std::erase_if(matured_command_tooltips_, [&tree](const std::uint64_t identity) {
        return tree.find_identity(identity) == nullptr;
    });
    std::erase_if(editors_, [&tree](const auto& entry) {
        return tree.find_identity(entry.first) == nullptr;
    });
    std::erase_if(text_navigation_, [&tree](const auto& entry) {
        return tree.find_identity(entry.first) == nullptr;
    });
    std::erase_if(static_text_ranges_, [this, &tree](const auto& entry) {
        const RetainedNode* node = tree.find_identity(entry.first);
        return node == nullptr || !static_text_selectable(*node);
    });
    for (auto& [identity, range] : static_text_ranges_) {
        const RetainedNode* node = tree.find_identity(identity);
        const std::optional<std::string_view> text = node != nullptr
            ? static_text_value(*node)
            : std::nullopt;
        if (!text.has_value()) continue;
        const auto clamp_boundary = [&text](std::size_t offset) {
            offset = std::min(offset, text->size());
            while (offset > 0U && offset < text->size() &&
                   (static_cast<unsigned char>((*text)[offset]) & 0xC0U) == 0x80U) {
                --offset;
            }
            return offset;
        };
        const StaticTextRange clamped{
            clamp_boundary(range.anchor), clamp_boundary(range.focus),
        };
        if (range.anchor != clamped.anchor || range.focus != clamped.focus) {
            range = clamped;
            static_cast<void>(tree.mark(identity, DirtyReason::input));
        }
    }
    for (const std::string& type : widgets_.text_editable_types()) {
        const std::vector<RetainedNode*>* editable_nodes = tree.find_type(type);
        if (editable_nodes == nullptr) continue;
        const WidgetLifecycle* lifecycle = widgets_.find(type);
        if (lifecycle == nullptr) continue;
        for (RetainedNode* node : *editable_nodes) {
            if (lifecycle->input.text_edit_mode == WidgetTextEditMode::static_text) {
                // Static selection has its own sparse read-only model. It must never enter the
                // writable editor/IME map, including when RichText owns activatable links.
                editors_.erase(node->identity());
                continue;
            }
            const runtime::Value* controlled = nullptr;
            std::string next;
            if (lifecycle->input.text_edit_mode == WidgetTextEditMode::numeric) {
                controlled = scalar_property(*node, "value");
                if (controlled == nullptr || controlled->number() == nullptr) {
                    controlled = node->retained_value("$value");
                }
                if (controlled == nullptr || controlled->number() == nullptr) {
                    controlled = scalar_property(*node, "defaultValue");
                }
                if (controlled != nullptr && controlled->number() != nullptr) {
                    next = runtime::display_string(*controlled);
                }
            } else {
                controlled = scalar_property(*node, "text");
                if (controlled == nullptr || controlled->string() == nullptr) {
                    controlled = node->retained_value("$text");
                }
                if (controlled != nullptr && controlled->string() != nullptr) {
                    next = *controlled->string();
                }
            }
            auto [editor, inserted] = editors_.try_emplace(node->identity(), next);
            TextEditorMutation mutation;
            if (!inserted) mutation = editor->second.reconcile_controlled(next);
            const runtime::Value* read_only = scalar_property(*node, "readOnly");
            if (read_only != nullptr && read_only->boolean() != nullptr &&
                *read_only->boolean()) {
                mutation = mutation + editor->second.cancel_preedit();
            }
            if (mutation.changed()) {
                text_navigation_.erase(node->identity());
                static_cast<void>(tree.mark(node->identity(), DirtyReason::editor));
            }
        }
    }
    if (static_text_selection_.has_value() &&
        (tree.find_identity(static_text_selection_->anchor_identity) == nullptr ||
         !static_text_ranges_.contains(static_text_selection_->anchor_identity))) {
        static_text_selection_.reset();
    }
    if (restore_focus_key.has_value()) {
        if (const RetainedNode* restore = tree.find_key(*restore_focus_key); restore != nullptr) {
            focus(*restore, "programmatic", result);
        }
    }
    sync_modal_focus(result);
    sanitize_focus(result);
    return result;
}

InputOperationResult InputRouter::after_layout() {
    InputOperationResult result;
    if (tree_ == nullptr || layout_ == nullptr) {
        host_services_->request_ime(host_service_owner_, std::nullopt);
        return result;
    }
    for (auto& [pointer_id, session] : drag_sessions_) {
        static_cast<void>(pointer_id);
        if (session.active) {
            synchronize_drag_target(session, session.position, false, result);
        }
    }
    std::vector<PendingReveal> pending = std::move(pending_reveals_);
    pending_reveals_.clear();
    const auto reveal_virtual_item = [this, &result](const PendingReveal& reveal) {
        for (const auto& [identity, record] : layout_->records) {
            if (!record.virtual_item_extents.has_value() ||
                !record.virtual_axis.has_value() || !record.viewport.has_value()) {
                continue;
            }
            RetainedNode* owner = tree_->find_identity(identity);
            if (owner == nullptr || (reveal.scroll_key.has_value() &&
                owner->description().key != reveal.scroll_key)) {
                continue;
            }
            std::optional<std::size_t> item_index;
            if (record.virtual_items != nullptr) {
                item_index = record.virtual_items->index_of_key(reveal.key);
            } else if (const auto direct = std::ranges::find(
                           record.virtual_item_keys,
                           reveal.key
                       ); direct != record.virtual_item_keys.end()) {
                item_index = record.virtual_item_key_start + static_cast<std::size_t>(
                    direct - record.virtual_item_keys.begin()
                );
            }
            if (!item_index.has_value() && record.virtual_item_members != nullptr) {
                for (std::size_t index = 0U;
                     index < record.virtual_item_members->size(); ++index) {
                    if (std::ranges::contains(
                            (*record.virtual_item_members)[index], reveal.key
                        )) {
                        item_index = index;
                        break;
                    }
                }
            }
            if (!item_index.has_value() ||
                *item_index >= record.virtual_item_extents->size()) {
                continue;
            }

            const bool vertical = *record.virtual_axis == LayoutAxis::vertical;
            const double viewport_extent = vertical
                                               ? record.viewport->height
                                               : record.viewport->width;
            const double leading_content_inset = vertical
                ? std::max(0.0, record.content_bounds.y - record.viewport->y)
                : std::max(0.0, record.content_bounds.x - record.viewport->x);
            const double start = leading_content_inset +
                record.virtual_item_extents->start(*item_index);
            const double end = start + record.virtual_item_extents->extent(*item_index);
            const double padding = std::clamp(
                reveal.padding,
                0.0,
                viewport_extent * 0.5
            );
            Point next = record.scroll_offset;
            double& offset = vertical ? next.y : next.x;
            if (start < offset + padding) {
                offset = std::max(0.0, start - padding);
            } else if (end > offset + viewport_extent - padding) {
                offset = std::max(0.0, end - viewport_extent + padding);
            }
            static_cast<void>(set_scroll_offset(*owner, next, result));
            return true;
        }
        return false;
    };
    for (const PendingReveal& reveal : pending) {
        RetainedNode* target = tree_->find_key(reveal.key);
        if (target == nullptr) {
            static_cast<void>(reveal_virtual_item(reveal));
            constexpr std::size_t maximum_attempts = 3U;
            if (reveal.attempts + 1U < maximum_attempts) {
                PendingReveal retry = reveal;
                ++retry.attempts;
                pending_reveals_.push_back(std::move(retry));
            } else {
                pending_diagnostics_.push_back(runtime::RuntimeDiagnostic{
                    "STRATA.UI.REVEAL_TARGET_MISSING",
                    "Post-layout reveal target '" + reveal.key + "' did not materialize.",
                    {},
                    "attached keyed node or lazy collection item",
                    runtime::DiagnosticSeverity::warning,
                    std::nullopt,
                });
            }
            continue;
        }
        if (reveal.focus && focusable(*target)) {
            focus(*target, "programmatic", result);
        }
    }
    route_active_lifecycle_hooks(true, result);
    std::optional<runtime::HostServiceRect> ime_cursor;
    if (focused_.has_value() && ime_cursor_rect_resolver_) {
        RetainedNode* const node = tree_->find_identity(*focused_);
        const LayoutRecord* const record = node != nullptr
            ? layout_->find(node->identity())
            : nullptr;
        const auto editor = node != nullptr ? editors_.find(node->identity()) : editors_.end();
        const WidgetLifecycle* const lifecycle = node != nullptr
            ? widgets_.find(node->description().type)
            : nullptr;
        const runtime::Value* const read_only = node != nullptr
            ? scalar_property(*node, "readOnly")
            : nullptr;
        const bool editable = lifecycle != nullptr &&
            lifecycle->input.text_edit_mode != WidgetTextEditMode::none &&
            lifecycle->input.text_edit_mode != WidgetTextEditMode::static_text &&
            !(read_only != nullptr && read_only->boolean() != nullptr &&
              *read_only->boolean());
        if (editable && record != nullptr && editor != editors_.end()) {
            const EditorSnapshot snapshot = editor->second.snapshot();
            ime_cursor = ime_cursor_rect_resolver_(
                *node,
                *record,
                TextEditorSnapshot{
                    snapshot.text,
                    snapshot.caret,
                    snapshot.selection_start,
                    snapshot.selection_end,
                    snapshot.composition.has_value()
                        ? std::optional<std::string_view>(*snapshot.composition)
                        : std::nullopt,
                    snapshot.composition_selection_start,
                    snapshot.composition_selection_end,
                }
            );
        }
    }
    host_services_->request_ime(host_service_owner_, ime_cursor);
    return result;
}

void InputRouter::publish_layout(const LayoutResult& layout) noexcept {
    layout_ = &layout;
}

void InputRouter::publish_motion(const MotionRuntime& motion) noexcept { motion_ = &motion; }

void InputRouter::publish_commands(CommandIndex& commands) noexcept { commands_ = &commands; }

void InputRouter::invalidate_host_geometry() noexcept {
    host_services_->invalidate_ime_geometry(host_service_owner_);
    if (frame_invalidator_) frame_invalidator_();
}

void InputRouter::publish_frame_time(const std::int64_t frame_time_nanos) {
    frame_time_nanos_ = frame_time_nanos;
    if (tree_ == nullptr) return;
    for (const auto& [identity, started] : hover_started_nanos_) {
        if (matured_command_tooltips_.contains(identity) ||
            frame_time_nanos_ < started ||
            frame_time_nanos_ - started < input_config_.command_tooltip_delay_nanos ||
            !command_tooltip_candidate(identity)) {
            continue;
        }
        matured_command_tooltips_.insert(identity);
        static_cast<void>(tree_->mark(identity, DirtyReason::input));
    }
    update_tooltip_disclosures();
}

JsonValue InputRouter::source(const RetainedNode& node) const {
    return object({
        {"componentType", JsonValue(node.description().type)},
        {"nodeKey", node.description().key.has_value() ? JsonValue(*node.description().key) : JsonValue{}},
        {"structuralPath", JsonValue(std::string(node.structural_path()))},
        {"surfaceId", JsonValue(public_surface_id_)},
    });
}

void InputRouter::set_focus_visibility(const bool visible) {
    if (focus_highlight_visible_ == visible) return;
    focus_highlight_visible_ = visible;
    if (focused_.has_value() && tree_ != nullptr) {
        static_cast<void>(tree_->mark(*focused_, DirtyReason::input));
    }
}

void InputRouter::focus(
    const RetainedNode& node,
    const std::string_view reason,
    InputOperationResult& result
) {
    if (inside_native_presentation(node)) return;
    if (!within_focus_containment(node)) return;
    if (focused_ == node.identity()) return;
    if (focused_.has_value()) {
        if (RetainedNode* previous = tree_->find_identity(*focused_); previous != nullptr) {
            static_cast<void>(route_event(
                previous, nullptr, InputEventKind::blur,
                nullptr, nullptr, nullptr, nullptr, nullptr, result
            ));
            commit_editor(*previous, result);
            static_cast<void>(tree_->mark(previous->identity(), DirtyReason::input));
            note_field_blur(*previous);
            JsonValue event = object({
                {"action", JsonValue{}},
                {"focused", JsonValue(false)},
                {"reason", JsonValue(std::string(reason))},
                {"source", source(*previous)},
                {"type", JsonValue("focus-changed")},
            });
            result.action_outcomes.push_back(no_action_outcome(event));
            result.events.push_back(std::move(event));
        }
    }
    focused_ = node.identity();
    update_tooltip_disclosures();
    static_cast<void>(tree_->mark(node.identity(), DirtyReason::input));
    static_cast<void>(route_event(
        const_cast<RetainedNode*>(&node), nullptr, InputEventKind::focus,
        nullptr, nullptr, nullptr, nullptr, nullptr, result
    ));
    if (reason != "pointer") reveal_focus(node, result);
    JsonValue event = object({
        {"action", JsonValue{}},
        {"focused", JsonValue(true)},
        {"reason", JsonValue(std::string(reason))},
        {"source", source(node)},
        {"type", JsonValue("focus-changed")},
    });
    result.action_outcomes.push_back(no_action_outcome(event));
    result.events.push_back(std::move(event));
}

void InputRouter::clear_focus(
    const std::string_view reason,
    InputOperationResult& result
) {
    // Hiding/cancelling a Surface may prevent any later after_layout publication. Releasing here
    // is synchronous and HostServices ignores inactive sibling owners.
    host_services_->request_ime(host_service_owner_, std::nullopt);
    if (!focused_.has_value()) return;
    RetainedNode* previous = tree_ != nullptr ? tree_->find_identity(*focused_) : nullptr;
    focused_.reset();
    update_tooltip_disclosures();
    if (previous == nullptr) return;
    static_cast<void>(route_event(
        previous, nullptr, InputEventKind::blur,
        nullptr, nullptr, nullptr, nullptr, nullptr, result
    ));
    commit_editor(*previous, result);
    static_cast<void>(tree_->mark(previous->identity(), DirtyReason::input));
    note_field_blur(*previous);
    JsonValue event = object({
        {"action", JsonValue{}},
        {"focused", JsonValue(false)},
        {"reason", JsonValue(std::string(reason))},
        {"source", source(*previous)},
        {"type", JsonValue("focus-changed")},
    });
    result.action_outcomes.push_back(no_action_outcome(event));
    result.events.push_back(std::move(event));
}

void InputRouter::hover_route(const RetainedNode* target) {
    std::size_t route_size = 0U;
    bool unchanged = true;
    for (const RetainedNode* current = target; current != nullptr; current = current->parent()) {
        ++route_size;
        unchanged = unchanged && hovered_.contains(current->identity());
    }
    if (unchanged && route_size == hovered_.size()) return;

    std::set<std::uint64_t> next;
    for (const RetainedNode* current = target; current != nullptr; current = current->parent()) {
        next.insert(current->identity());
    }
    for (const std::uint64_t identity : hovered_) {
        if (!next.contains(identity)) {
            static_cast<void>(tree_->mark(identity, DirtyReason::input));
            hover_started_nanos_.erase(identity);
            matured_command_tooltips_.erase(identity);
        }
    }
    for (const std::uint64_t identity : next) {
        if (!hovered_.contains(identity)) {
            static_cast<void>(tree_->mark(identity, DirtyReason::input));
            hover_started_nanos_.insert_or_assign(identity, frame_time_nanos_);
            matured_command_tooltips_.erase(identity);
        }
    }
    hovered_ = std::move(next);
    update_tooltip_disclosures();
}

std::shared_ptr<const runtime::ActionValue> InputRouter::activation_action(
    const RetainedNode& node,
    const std::string_view property
) const {
    const auto found = node.description().properties.find(property);
    if (found != node.description().properties.end() && found->second.action() != nullptr) {
        const std::shared_ptr<const runtime::ActionValue>& value = *found->second.action();
        if (value != nullptr && value->action != nullptr) return value;
        return nullptr;
    }
    const runtime::Value* default_action = scalar_property(node, "$defaultAction");
    const std::string* default_id = string_value(default_action);
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    if (lifecycle != nullptr && lifecycle->describe.default_action_factory != nullptr) {
        const std::shared_ptr<const runtime::ActionValue> resolved =
            lifecycle->describe.default_action_factory(
                node,
                application_.bundle()->action_registry()
            );
        if (resolved != nullptr && resolved->action != nullptr) return resolved;
    }
    std::string_view fallback_id = default_id != nullptr ? std::string_view(*default_id)
                                                         : std::string_view{};
    if (fallback_id.empty()) {
        if (lifecycle != nullptr) fallback_id = lifecycle->input.fallback_action;
    }
    const auto resolved_contract = fallback_id.empty()
                                       ? nullptr
                                       : application_.bundle()->action_registry().contract(fallback_id);
    if (resolved_contract == nullptr) return nullptr;
    return std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
        std::make_shared<const runtime::Action>(resolved_contract),
        std::nullopt,
        {},
    });
}

std::optional<std::uint64_t> InputRouter::focused_identity() const noexcept { return focused_; }

std::optional<std::string_view> InputRouter::focused_key() const noexcept {
    if (!focused_.has_value() || tree_ == nullptr) return std::nullopt;
    const RetainedNode* node = tree_->find_identity(*focused_);
    return node != nullptr && node->description().key.has_value()
               ? std::optional<std::string_view>(*node->description().key)
               : std::nullopt;
}

RetainedNode* InputRouter::focus_boundary() const noexcept {
    return focus_containment_key_.has_value() && tree_ != nullptr
        ? tree_->find_key(*focus_containment_key_)
        : nullptr;
}

bool InputRouter::within_focus_containment(const RetainedNode& node) const noexcept {
    if (!focus_containment_key_.has_value()) return true;
    const RetainedNode* boundary = focus_boundary();
    return boundary != nullptr && descendant_of(node, *boundary);
}

bool InputRouter::set_focus_containment(
    const std::optional<std::string_view> key,
    InputOperationResult& result
) {
    if (!key.has_value()) {
        if (!focus_containment_key_.has_value()) return false;
        const std::optional<std::uint64_t> restore = focus_before_containment_;
        focus_containment_key_.reset();
        focus_before_containment_.reset();
        if (restore.has_value() && tree_ != nullptr) {
            if (RetainedNode* target = tree_->find_identity(*restore);
                target != nullptr && focusable(*target)) {
                focus(*target, "programmatic", result);
            }
        }
        if (frame_invalidator_) frame_invalidator_();
        return false;
    }
    if (key->empty() || tree_ == nullptr) return false;
    RetainedNode* boundary = tree_->find_key(*key);
    if (boundary == nullptr) return false;
    if (!focus_containment_key_.has_value()) focus_before_containment_ = focused_;
    focus_containment_key_ = std::string(*key);
    RetainedNode* current = focused_.has_value() ? tree_->find_identity(*focused_) : nullptr;
    if (current == nullptr || !descendant_of(*current, *boundary)) {
        const std::vector<RetainedNode*> candidates = focusable_nodes();
        if (!candidates.empty()) focus(*candidates.front(), "programmatic", result);
        else clear_focus("programmatic", result);
    }
    if (frame_invalidator_) frame_invalidator_();
    return true;
}

bool InputRouter::focus_contained() const noexcept {
    return focus_containment_key_.has_value() && focus_boundary() != nullptr;
}

bool InputRouter::focused(const std::uint64_t identity) const noexcept { return focused_ == identity; }
bool InputRouter::focus_visible(const std::uint64_t identity) const noexcept {
    return focus_highlight_visible_ && focused_ == identity;
}
bool InputRouter::hovered(const std::uint64_t identity) const noexcept {
    return hovered_.contains(identity);
}
bool InputRouter::command_tooltip_ready(const std::uint64_t identity) const noexcept {
    return hovered_.contains(identity) && matured_command_tooltips_.contains(identity) &&
           command_tooltip_candidate(identity);
}
bool InputRouter::active(const std::uint64_t identity) const noexcept { return active_ == identity; }

const std::string* InputRouter::edited_text(const std::uint64_t identity) const noexcept {
    const auto found = editors_.find(identity);
    return found != editors_.end() ? &found->second.text() : nullptr;
}

std::optional<TextEditorSnapshot> InputRouter::editor_snapshot(
    const std::uint64_t identity
) const noexcept {
    const auto found = editors_.find(identity);
    if (found == editors_.end()) return std::nullopt;
    const EditorSnapshot snapshot = found->second.snapshot();
    return TextEditorSnapshot{
        snapshot.text,
        snapshot.caret,
        snapshot.selection_start,
        snapshot.selection_end,
        snapshot.composition,
        snapshot.composition_selection_start,
        snapshot.composition_selection_end,
    };
}

std::optional<StaticTextSelectionSnapshot> InputRouter::static_text_selection_snapshot(
    const std::uint64_t identity
) const noexcept {
    const auto range = static_text_ranges_.find(identity);
    const RetainedNode* node = range != static_text_ranges_.end() && tree_ != nullptr
        ? tree_->find_identity(identity)
        : nullptr;
    const std::optional<std::string_view> text = node != nullptr
        ? static_text_value(*node)
        : std::nullopt;
    if (range == static_text_ranges_.end() || !text.has_value()) return std::nullopt;
    return StaticTextSelectionSnapshot{
        *text, range->second.focus, range->second.anchor, range->second.focus,
    };
}

std::vector<std::string> InputRouter::pending_navigation_targets() const {
    std::vector<std::string> result;
    result.reserve(pending_reveals_.size());
    for (const PendingReveal& reveal : pending_reveals_) result.push_back(reveal.key);
    return result;
}

const StatusFeedbackService& InputRouter::status_feedback() const noexcept {
    return status_feedback_;
}

NotificationService& InputRouter::notifications() noexcept { return notifications_; }
const NotificationService& InputRouter::notifications() const noexcept {
    return notifications_;
}
} // namespace strata::ui
