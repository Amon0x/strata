#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ui/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {

struct WidgetDragInteraction final {
    std::optional<std::string> source_key;
    std::optional<std::string> target_key;
    Point position;
    std::string operation;
    std::string placement;
};

/** Widget-local capabilities layered over the shared phased input dispatch context. */
class WidgetInputScope final {
  public:
    WidgetInputScope(InputRouter& router, RetainedNode& node, InputOperationResult& result,
                     std::string_view key, KeyModifiers modifiers,
                     const PointerInputEvent* pointer = nullptr,
                     RetainedNode* pointer_target = nullptr, std::size_t click_count = 0U,
                     std::optional<WidgetSubtarget> subtarget = std::nullopt,
                     std::string_view input_text = {},
                     InputDispatchContext* dispatch = nullptr) noexcept;

    [[nodiscard]] RetainedNode& node() noexcept;
    [[nodiscard]] const RetainedNode& node() const noexcept;
    [[nodiscard]] std::string_view key() const noexcept;
    [[nodiscard]] std::string_view input_text() const noexcept;
    [[nodiscard]] const KeyModifiers& modifiers() const noexcept;
    [[nodiscard]] const PointerInputEvent* pointer() const noexcept;
    [[nodiscard]] const ScrollInputEvent* scroll() const noexcept;
    [[nodiscard]] RetainedNode* pointer_target() const noexcept;
    [[nodiscard]] std::size_t click_count() const noexcept;
    [[nodiscard]] const WidgetSubtarget* subtarget() const noexcept;
    [[nodiscard]] InputDispatchContext* dispatch() const noexcept;
    [[nodiscard]] InputEventPhase phase() const noexcept;
    [[nodiscard]] InputEventKind kind() const noexcept;
    [[nodiscard]] bool press_moved_beyond_slop() const noexcept;
    [[nodiscard]] GestureClaimState gesture_claim_state() const noexcept;
    void consume() noexcept;
    void stop_propagation() noexcept;
    [[nodiscard]] bool claim_gesture() noexcept;
    [[nodiscard]] bool cancel_gesture() noexcept;
    [[nodiscard]] std::vector<WidgetSubtarget> subtargets() const;
    [[nodiscard]] std::optional<std::size_t> text_offset(std::string_view text) const;
    [[nodiscard]] std::int64_t frame_time_nanos() const noexcept;
    /** Active drag whose current target is this widget or one of its descendants. */
    [[nodiscard]] std::optional<WidgetDragInteraction> active_drag() const;
    [[nodiscard]] const LayoutRecord* layout() const noexcept;
    [[nodiscard]] const LayoutRecord* layout(const RetainedNode& node) const noexcept;
    [[nodiscard]] const LayoutResult* layout_result() const noexcept;
    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] const CommandIndex* command_index() const noexcept;
    [[nodiscard]] NotificationService& notifications() noexcept;
    void synchronize_modal_focus();
    [[nodiscard]] const runtime::Value* property(std::string_view name) const noexcept;
    [[nodiscard]] const runtime::Value* retained(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const std::byte> retained_bytes(std::string_view name) const noexcept;
    [[nodiscard]] bool effective_boolean(std::string_view controlled,
                                         std::string_view retained_name, std::string_view initial,
                                         bool fallback) const noexcept;
    [[nodiscard]] double effective_number(std::string_view controlled,
                                          std::string_view retained_name, std::string_view initial,
                                          double fallback) const noexcept;
    [[nodiscard]] double number(std::string_view name, double fallback) const noexcept;
    [[nodiscard]] std::int64_t duration_nanos(std::string_view name,
                                              std::int64_t fallback) const noexcept;
    [[nodiscard]] std::string string(std::string_view name, std::string fallback = {}) const;
    /** Reveals a vertical item range inside this widget's scroll viewport. */
    [[nodiscard]] bool reveal_vertical(double item_start, double item_end,
                                       double sticky_leading_inset = 0.0);
    [[nodiscard]] bool scroll_by(double delta_x, double delta_y);
    [[nodiscard]] const std::string* editor_text() const noexcept;
    /** Inserts through the surface-owned editor, preserving filtering, undo, and dirty tracking. */
    [[nodiscard]] bool insert_editor_text(std::string_view value);
    /** Clears the surface-owned draft as one undoable editor mutation. */
    [[nodiscard]] bool clear_editor_text();
    /** Synchronizes widget-owned text without emitting a second editor change event. */
    void synchronize_editor_text(std::string_view value, bool move_caret_to_end = false);

    void set_retained(std::string name, runtime::Value value, DirtyReason reason);
    void set_retained_bytes(std::string name, std::span<const std::byte> value, DirtyReason reason);
    void set_presentation(std::string name, runtime::Value value);
    void set_paint(std::string name, runtime::Value value);
    void set_input(std::string name, runtime::Value value);
    void set_input_bytes(std::string name, std::span<const std::byte> value);
    void invalidate(DirtyReason reason);
    [[nodiscard]] bool request_frame(WidgetFrameCost cost = WidgetFrameCost::paint);
    void cancel_frame() noexcept;
    void set_event_count(std::size_t count) noexcept;
    void activated(std::string_view action_property);
    void boolean_changed(std::string_view action_property, bool value);
    void number_changed(std::string_view action_property, double value);
    void value_changed(std::string_view action_property, std::string_view event_kind,
                       runtime::Value event_value);
    void dispatch_action(std::shared_ptr<const runtime::ActionValue> action,
                         std::string_view event_kind,
                         runtime::Value event_value = runtime::Value{});
    /** Emits an extension-owned action through the same registry/dispatcher as authored actions. */
    [[nodiscard]] bool emit_action(std::string_view action_id, runtime::Value payload,
                                   std::string_view event_kind,
                                   runtime::Value event_value = runtime::Value{});
    [[nodiscard]] bool invoke_command(std::string_view id);
    void emit_event(std::string_view event_kind, runtime::Value event_value = runtime::Value{});

  private:
    [[nodiscard]] std::shared_ptr<const runtime::ActionValue>
    action(std::string_view property_name) const;

    InputRouter& router_;
    RetainedNode& node_;
    InputOperationResult& result_;
    std::string_view key_;
    KeyModifiers modifiers_;
    const PointerInputEvent* pointer_ = nullptr;
    RetainedNode* pointer_target_ = nullptr;
    std::size_t click_count_ = 0U;
    std::optional<WidgetSubtarget> subtarget_;
    std::string_view input_text_;
    InputDispatchContext* dispatch_ = nullptr;
};

void register_primitive_widget_inputs(WidgetRegistry& registry);
void register_control_widget_inputs(WidgetRegistry& registry);
void register_shell_widget_inputs(WidgetRegistry& registry);
void register_collection_widget_inputs(WidgetRegistry& registry);

} // namespace strata::ui
