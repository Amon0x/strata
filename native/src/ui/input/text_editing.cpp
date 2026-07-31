#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/input/detail.hpp"
#include "ui/status.hpp"
#include "ui/text_geometry.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

[[nodiscard]] bool text_read_only(const RetainedNode& node) noexcept {
    if (node.description().type == "Text" || node.description().type == "RichText") {
        return true;
    }
    return boolean_value(scalar_property(node, "readOnly"), false);
}

[[nodiscard]] TextInputFilter text_filter(
    const RetainedNode& node,
    const WidgetTextEditMode mode
) {
    const std::string* value = string_value(scalar_property(node, "filter"));
    const std::string name = value != nullptr ? lower_ascii(*value) : std::string{};
    if (name == "integer") return TextInputFilter::integer;
    if (name == "decimal") return TextInputFilter::decimal;
    if (name == "letters") return TextInputFilter::letters;
    if (name == "alphanumeric") return TextInputFilter::alphanumeric;
    return mode == WidgetTextEditMode::numeric ? TextInputFilter::decimal
                                                : TextInputFilter::any;
}

[[nodiscard]] TextCommitFormat text_commit_format(const RetainedNode& node) {
    const std::string* value = string_value(scalar_property(node, "commitFormat"));
    const std::string name = value != nullptr ? lower_ascii(*value) : std::string{};
    if (name == "trim") return TextCommitFormat::trim;
    if (name == "uppercase") return TextCommitFormat::uppercase;
    if (name == "lowercase") return TextCommitFormat::lowercase;
    return TextCommitFormat::none;
}

[[nodiscard]] TextEditorConfig editor_config(
    const RetainedNode& node,
    const WidgetLifecycle* lifecycle
) {
    const WidgetTextEditMode mode = lifecycle != nullptr
                                        ? lifecycle->input.text_edit_mode
                                        : WidgetTextEditMode::none;
    TextEditorConfig config;
    config.multiline = mode == WidgetTextEditMode::multi_line;
    config.filter = text_filter(node, mode);
    config.commit_format = text_commit_format(node);
    if (const runtime::Value* maximum = scalar_property(node, "maxLength");
        maximum != nullptr && maximum->number() != nullptr &&
        std::isfinite(*maximum->number()) && *maximum->number() >= 0.0 &&
        *maximum->number() <= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        config.max_code_points = static_cast<std::size_t>(*maximum->number());
    }
    return config;
}

[[nodiscard]] bool command_key(const KeyModifiers modifiers) noexcept {
    return modifiers.control || modifiers.super_key;
}

[[nodiscard]] bool editor_reserves_key(
    const std::string_view key,
    const KeyModifiers modifiers
) noexcept {
    if (command_key(modifiers)) {
        return key == "a" || key == "c" || key == "x" || key == "v" ||
               key == "z" || key == "y";
    }
    if (modifiers.control || modifiers.super_key || modifiers.alt) return false;
    return key == "left" || key == "right" || key == "up" || key == "down" ||
           key == "home" || key == "end" || key == "backspace" || key == "delete" ||
           key == "enter" || key == "space" || key.size() == 1U;
}

} // namespace

void InputRouter::record_editor_mutation(
    RetainedNode& node,
    const TextEditorMutation& mutation,
    InputOperationResult& result
) {
    if (!mutation.changed()) return;
    if (tree_ != nullptr) static_cast<void>(tree_->mark(node.identity(), DirtyReason::editor));
    if (frame_invalidator_) frame_invalidator_();
    if (!mutation.text_changed) return;

    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    if (lifecycle != nullptr && !lifecycle->input.editor_emits_change) return;

    const auto editor = editors_.find(node.identity());
    if (editor == editors_.end()) return;
    std::shared_ptr<const runtime::ActionValue> action;
    const auto property_action = node.description().properties.find("onChange");
    if (property_action != node.description().properties.end() &&
        property_action->second.action() != nullptr && *property_action->second.action() != nullptr) {
        action = *property_action->second.action();
    }
    if (action == nullptr) {
        const auto contract = application_.bundle()->action_registry().contract("text-edit");
        if (contract != nullptr) {
            action = std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
                std::make_shared<const runtime::Action>(contract),
                std::nullopt,
                {},
            });
        }
    }
    const std::string& text = editor->second.text();
    JsonValue event = object({
        {"action", action != nullptr ? canonical_action(*action, node) : JsonValue{}},
        {"source", source(node)},
        {"text", JsonValue(text)},
        {"type", JsonValue("text-changed")},
    });
    note_field_change(node);
    emit(std::move(event), action, node, runtime::Value(text), result);
}

bool InputRouter::insert_editor_text(
    RetainedNode& node,
    const std::string_view value,
    InputOperationResult& result
) {
    const auto editor = editors_.find(node.identity());
    if (editor == editors_.end()) return false;
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    const TextEditorMutation mutation = text_read_only(node)
        ? editor->second.cancel_preedit()
        : editor->second.insert(value, editor_config(node, lifecycle), frame_time_nanos_);
    if (mutation.text_changed || mutation.selection_changed) {
        text_navigation_.erase(node.identity());
    }
    record_editor_mutation(node, mutation, result);
    return mutation.changed();
}

bool InputRouter::clear_editor_text(
    RetainedNode& node,
    InputOperationResult& result
) {
    const auto editor = editors_.find(node.identity());
    if (editor == editors_.end()) return false;
    TextEditorMutation mutation = editor->second.select_all();
    mutation = mutation + editor->second.erase_selection(frame_time_nanos_);
    if (mutation.changed()) text_navigation_.erase(node.identity());
    record_editor_mutation(node, mutation, result);
    return mutation.changed();
}

void InputRouter::synchronize_editor_text(
    RetainedNode& node,
    const std::string_view value,
    const bool move_caret_to_end
) {
    const auto editor = editors_.find(node.identity());
    if (editor == editors_.end()) return;
    TextEditorMutation mutation = editor->second.reconcile_controlled(value);
    if (move_caret_to_end) mutation = mutation + editor->second.move_end(false);
    if (!mutation.changed()) return;
    text_navigation_.erase(node.identity());
    if (tree_ != nullptr) static_cast<void>(tree_->mark(node.identity(), DirtyReason::editor));
    if (frame_invalidator_) frame_invalidator_();
}

void InputRouter::commit_editor(RetainedNode& node, InputOperationResult& result) {
    const auto editor = editors_.find(node.identity());
    if (editor == editors_.end()) return;
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    const TextEditorMutation mutation = text_read_only(node)
                                            ? editor->second.cancel_preedit()
                                            : editor->second.commit_format(
                                                  editor_config(node, lifecycle).commit_format
                                              );
    if (mutation.text_changed || mutation.selection_changed) {
        text_navigation_.erase(node.identity());
    }
    record_editor_mutation(node, mutation, result);
}

std::optional<std::size_t> InputRouter::visual_text_navigation_offset(
    const RetainedNode& node,
    const std::string_view text,
    const std::size_t caret,
    const std::string_view key
) {
    if (!text_layout_resolver_) return std::nullopt;
    const TextLayout layout = text_layout_resolver_(node, text, TextLayoutOptions{});
    if (layout.lines.empty()) return std::nullopt;
    const std::size_t utf16_caret = utf16_offset_for_utf8_byte(text, caret);
    const auto retained = text_navigation_.find(node.identity());
    const std::optional<std::size_t> preferred_line =
        retained != text_navigation_.end() && retained->second.caret == caret
            ? std::optional<std::size_t>(retained->second.line)
            : std::nullopt;
    const std::size_t line = text_layout_caret_line(
        layout, utf16_caret, preferred_line
    );
    std::size_t next_line = line;
    std::size_t next_utf16 = utf16_caret;
    std::optional<double> x_goal;
    if (key == "home" || key == "end") {
        next_utf16 = text_layout_line_edge_offset(layout, line, key == "end");
    } else if (key == "up" || key == "down") {
        x_goal = retained != text_navigation_.end() &&
                retained->second.caret == caret &&
                retained->second.vertical_x_goal.has_value()
            ? retained->second.vertical_x_goal
            : std::optional<double>(shaped_caret_x(layout.shaped, line, utf16_caret));
        next_line = key == "up"
            ? (line == 0U ? 0U : line - 1U)
            : std::min(line + 1U, layout.lines.size() - 1U);
        next_utf16 = text_layout_line_offset_at_x(layout, next_line, *x_goal);
    } else {
        return std::nullopt;
    }
    const std::size_t next = utf8_byte_for_utf16_offset(text, next_utf16);
    text_navigation_.insert_or_assign(
        node.identity(), TextNavigationState{next, next_line, x_goal}
    );
    return next;
}

InputOperationResult InputRouter::key(
    const std::string_view key_value,
    const KeyModifiers modifiers
) {
    return key(KeyInputEvent{
        std::string(key_value), modifiers, KeyEventType::press, frame_time_nanos_,
    }, false);
}

InputOperationResult InputRouter::key(
    KeyInputEvent event,
    const bool navigation_traversal_repeat
) {
    InputOperationResult result;
    result.injected_events = 1U;
    result.processed_events = 1U;
    if (event.type != KeyEventType::release) set_focus_visibility(true);
    const std::string normalized_key = lower_ascii(event.key);
    const KeyModifiers modifiers = event.modifiers;
    RetainedNode* node = focused_.has_value() && tree_ != nullptr
        ? tree_->find_identity(*focused_)
        : nullptr;
    InputDispatchState dispatch;
    if (node != nullptr) {
        dispatch = route_key_event(node, event, result);
        if (dispatch.consumed) return result;
    }
    // Releases participate in generic propagation but never repeat press/default policy.
    if (event.type == KeyEventType::release) return result;
    if (normalized_key == "escape" && dismiss_topmost(result)) return result;
    if (normalized_key == "escape" && cancel_active_drag(result)) return result;
    if ((event.type == KeyEventType::press ||
         (event.type == KeyEventType::repeat && navigation_traversal_repeat)) &&
        normalized_key == "tab") {
        static_cast<void>(traverse_focus(modifiers.shift, result));
        return result;
    }
    const bool editing_owns_key = focused_.has_value() && tree_ != nullptr &&
        ((editors_.contains(*focused_) && editor_reserves_key(normalized_key, modifiers)) ||
         (node != nullptr && static_text_selectable(*node) &&
          (normalized_key == "left" || normalized_key == "right" ||
           normalized_key == "up" || normalized_key == "down" ||
           normalized_key == "home" || normalized_key == "end" ||
           (command_key(modifiers) &&
            (normalized_key == "a" || normalized_key == "c" ||
             normalized_key == "x" || normalized_key == "v" ||
             normalized_key == "z" || normalized_key == "y")))));
    if (route_command_shortcut(
            normalized_key, modifiers, event.type, editing_owns_key, result
        )) {
        return result;
    }
    if (!editing_owns_key && command_key(modifiers) &&
        (normalized_key == "z" || normalized_key == "y")) {
        const bool redo = normalized_key == "y" || modifiers.shift;
        const runtime::UndoStackStatus status = application_.undo().status(public_surface_id_);
        const std::optional<std::string>& label = redo ? status.redo_label : status.undo_label;
        const bool changed = application_.undo_state(public_surface_id_, redo);
        if (changed && label.has_value()) {
            status_feedback_.publish(
                std::string(redo ? "Redo " : "Undo ") + *label
            );
        }
        return result;
    }
    if (node == nullptr) return result;

    const auto editor = editors_.find(node->identity());
    bool editor_handled = false;
    if (editor != editors_.end()) {
        for (RetainedNode* current = node; current != nullptr; current = current->parent()) {
            const WidgetLifecycle* current_lifecycle = widgets_.find(current->description().type);
            if (current_lifecycle == nullptr || current_lifecycle->input.editor_key == nullptr) {
                continue;
            }
            WidgetInputScope scope(*this, *current, result, normalized_key, modifiers);
            if (current_lifecycle->input.editor_key(scope)) return result;
        }
        const std::string& key = normalized_key;
        const bool command = command_key(modifiers);
        const bool read_only = text_read_only(*node);
        const WidgetLifecycle* lifecycle = widgets_.find(node->description().type);
        const TextEditorConfig config = editor_config(*node, lifecycle);
        TextEditorMutation mutation;
        bool visual_navigation = false;

        if (command && key == "a") {
            if (static_text_node(*node) && select_all_static_text(*node, result)) {
                editor_handled = true;
                return result;
            }
            mutation = editor->second.select_all();
            editor_handled = true;
        } else if (command && key == "c") {
            if (!static_text_node(*node) || !copy_static_text_selection(*node)) {
                const std::string selected = editor->second.selected_text();
                if (!selected.empty()) static_cast<void>(host_services_->write_clipboard(selected));
            }
            editor_handled = true;
        } else if (command && key == "x") {
            const std::string selected = editor->second.selected_text();
            if (!read_only && !selected.empty()) {
                static_cast<void>(host_services_->write_clipboard(selected));
                mutation = editor->second.erase_selection(frame_time_nanos_);
            }
            editor_handled = true;
        } else if (command && key == "v") {
            const std::optional<std::string> clipboard = host_services_->read_clipboard();
            if (!read_only && clipboard.has_value()) {
                mutation = editor->second.insert(*clipboard, config, frame_time_nanos_);
            }
            editor_handled = true;
        } else if (command && key == "z") {
            if (!read_only) mutation = modifiers.shift ? editor->second.redo() : editor->second.undo();
            editor_handled = true;
        } else if (command && key == "y") {
            if (!read_only) mutation = editor->second.redo();
            editor_handled = true;
        } else if (key == "left") {
            text_navigation_.erase(node->identity());
            mutation = editor->second.move_left(command, modifiers.shift);
            editor_handled = true;
        } else if (key == "right") {
            text_navigation_.erase(node->identity());
            mutation = editor->second.move_right(command, modifiers.shift);
            editor_handled = true;
        } else if (key == "home") {
            const std::optional<std::size_t> visual = config.multiline && !command
                ? visual_text_navigation_offset(
                      *node, editor->second.text(), editor->second.snapshot().caret, key
                  )
                : std::nullopt;
            mutation = visual.has_value()
                ? editor->second.place_caret(*visual, modifiers.shift)
                : (command || !config.multiline)
                    ? editor->second.move_home(modifiers.shift)
                    : editor->second.move_line_home(modifiers.shift);
            visual_navigation = visual.has_value();
            editor_handled = true;
        } else if (key == "end") {
            const std::optional<std::size_t> visual = config.multiline && !command
                ? visual_text_navigation_offset(
                      *node, editor->second.text(), editor->second.snapshot().caret, key
                  )
                : std::nullopt;
            mutation = visual.has_value()
                ? editor->second.place_caret(*visual, modifiers.shift)
                : (command || !config.multiline)
                    ? editor->second.move_end(modifiers.shift)
                    : editor->second.move_line_end(modifiers.shift);
            visual_navigation = visual.has_value();
            editor_handled = true;
        } else if (key == "up") {
            const std::optional<std::size_t> visual = visual_text_navigation_offset(
                *node, editor->second.text(), editor->second.snapshot().caret, key
            );
            mutation = visual.has_value()
                ? editor->second.place_caret(*visual, modifiers.shift)
                : editor->second.move_up(modifiers.shift);
            visual_navigation = visual.has_value();
            editor_handled = true;
        } else if (key == "down") {
            const std::optional<std::size_t> visual = visual_text_navigation_offset(
                *node, editor->second.text(), editor->second.snapshot().caret, key
            );
            mutation = visual.has_value()
                ? editor->second.place_caret(*visual, modifiers.shift)
                : editor->second.move_down(modifiers.shift);
            visual_navigation = visual.has_value();
            editor_handled = true;
        } else if (key == "backspace") {
            if (!read_only) mutation = editor->second.backspace(command, frame_time_nanos_);
            editor_handled = true;
        } else if (key == "delete") {
            if (!read_only) mutation = editor->second.delete_forward(command, frame_time_nanos_);
            editor_handled = true;
        } else if (key == "enter") {
            if (!read_only && config.multiline) {
                mutation = editor->second.insert("\n", config, frame_time_nanos_);
            } else if (!read_only) {
                mutation = editor->second.commit_format(config.commit_format);
            }
            // Single-line editors commit first, then let an ancestor lifecycle (for example Form)
            // consume Enter. Multiline editors retain Enter as text input.
            editor_handled = config.multiline;
        }
        if (mutation.changed() && !visual_navigation && key != "left" && key != "right") {
            text_navigation_.erase(node->identity());
        }
        record_editor_mutation(*node, mutation, result);
    }

    if (!editor_handled) {
        editor_handled = move_static_text_selection(
            *node, normalized_key, modifiers, result
        );
    }

    if (!editor_handled) {
        bool widget_handled = false;
        const WidgetLifecycle* lifecycle = widgets_.find(node->description().type);
        for (RetainedNode* current = node; current != nullptr && !widget_handled;
             current = current->parent()) {
            const WidgetLifecycle* current_lifecycle = widgets_.find(current->description().type);
            if (current_lifecycle == nullptr || current_lifecycle->input.key == nullptr) continue;
            InputDispatchContext context(
                *this,
                *current,
                node,
                nullptr,
                current == node ? InputEventPhase::target : InputEventPhase::bubble,
                InputEventKind::key,
                dispatch,
                nullptr,
                nullptr,
                &event
            );
            WidgetInputScope scope(
                *this, *current, result, normalized_key, modifiers,
                nullptr, nullptr, 0U, std::nullopt, {}, &context
            );
            widget_handled = current_lifecycle->input.key(scope);
        }
        if (!widget_handled && scroll_focused_ancestor(normalized_key, result)) {
            widget_handled = true;
        }
        if (!widget_handled && lifecycle != nullptr && lifecycle->input.click != nullptr &&
            (normalized_key == "enter" || normalized_key == "space")) {
            activate(*node, result);
        } else if (!widget_handled && (
            normalized_key == "left" || normalized_key == "right" ||
            normalized_key == "up" || normalized_key == "down"
        )) {
            static_cast<void>(move_focus_spatial(normalized_key, result));
        }
    }
    return result;
}

InputOperationResult InputRouter::text(std::string value) {
    return text(TextInputEvent{std::move(value), frame_time_nanos_});
}

InputOperationResult InputRouter::text(TextInputEvent event) {
    InputOperationResult result;
    result.injected_events = 1U;
    result.processed_events = 1U;
    if (!focused_.has_value() || tree_ == nullptr) return result;
    RetainedNode* node = tree_->find_identity(*focused_);
    if (node == nullptr) return result;
    const InputDispatchState dispatch = route_event(
        node,
        nullptr,
        InputEventKind::text,
        nullptr,
        nullptr,
        nullptr,
        &event,
        nullptr,
        result
    );
    if (dispatch.consumed) return result;
    for (RetainedNode* current = node; current != nullptr; current = current->parent()) {
        const WidgetLifecycle* lifecycle = widgets_.find(current->description().type);
        if (lifecycle == nullptr || lifecycle->input.text == nullptr) continue;
        WidgetInputScope scope(
            *this, *current, result, {}, {}, nullptr, nullptr, 0U, std::nullopt, event.text
        );
        if (lifecycle->input.text(scope)) return result;
    }
    static_cast<void>(insert_editor_text(*node, event.text, result));
    return result;
}

InputOperationResult InputRouter::ime_preedit(
    std::string value,
    const std::size_t selection_start,
    const std::size_t selection_end
) {
    return ime_preedit(ImePreeditInputEvent{
        std::move(value), selection_start, selection_end, frame_time_nanos_,
    });
}

InputOperationResult InputRouter::ime_preedit(ImePreeditInputEvent event) {
    InputOperationResult result;
    result.injected_events = 1U;
    result.processed_events = 1U;
    if (!focused_.has_value() || tree_ == nullptr) return result;
    RetainedNode* node = tree_->find_identity(*focused_);
    if (node == nullptr) return result;
    const InputDispatchState dispatch = route_event(
        node,
        nullptr,
        InputEventKind::ime_preedit,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &event,
        result
    );
    if (dispatch.consumed) return result;
    const auto editor = editors_.find(node->identity());
    if (editor == editors_.end()) return result;
    const WidgetLifecycle* lifecycle = widgets_.find(node->description().type);
    const TextEditorConfig config = editor_config(*node, lifecycle);
    const TextEditorMutation mutation = text_read_only(*node)
                                            ? editor->second.cancel_preedit()
                                            : editor->second.set_preedit(
                                                  std::move(event.text),
                                                  event.selection_start,
                                                  event.selection_end,
                                                  config.multiline
                                              );
    record_editor_mutation(*node, mutation, result);
    return result;
}

bool InputRouter::copy_static_text_selection(RetainedNode& owner) {
    const std::optional<std::string_view> container = static_text_container(owner);
    std::string selected;
    const std::vector<RetainedNode*> nodes = container.has_value()
        ? static_text_nodes(*container)
        : std::vector<RetainedNode*>{&owner};
    for (RetainedNode* node : nodes) {
        const auto range = static_text_ranges_.find(node->identity());
        const std::optional<std::string_view> text = static_text_value(*node);
        if (range == static_text_ranges_.end() || !text.has_value()) continue;
        const std::size_t start = std::min(range->second.anchor, range->second.focus);
        const std::size_t end = std::max(range->second.anchor, range->second.focus);
        const std::string_view segment = text->substr(start, end - start);
        if (segment.empty()) continue;
        if (!selected.empty()) selected.push_back('\n');
        selected += segment;
    }
    if (selected.empty()) return false;
    static_cast<void>(host_services_->write_clipboard(selected));
    return true;
}

bool InputRouter::select_all_static_text(
    RetainedNode& owner,
    InputOperationResult& result
) {
    if (!static_text_selectable(owner)) return false;
    const std::optional<std::string_view> container = static_text_container(owner);
    const std::optional<std::string_view> text = static_text_value(owner);
    if (!text.has_value()) return false;
    std::vector<std::uint64_t> other_selections;
    other_selections.reserve(static_text_ranges_.size());
    for (const auto& [identity, range] : static_text_ranges_) {
        static_cast<void>(range);
        if (identity != owner.identity()) other_selections.push_back(identity);
    }
    for (const std::uint64_t identity : other_selections) {
        static_text_ranges_.erase(identity);
        text_navigation_.erase(identity);
        if (tree_ != nullptr && tree_->find_identity(identity) != nullptr) {
            static_cast<void>(tree_->mark(identity, DirtyReason::input));
        }
    }
    if (!other_selections.empty() && frame_invalidator_) frame_invalidator_();
    text_navigation_.erase(owner.identity());
    set_static_text_selection(owner, 0U, text->size(), result);
    static_text_selection_ = StaticTextSelectionSession{
        owner.identity(),
        0U,
        container.has_value()
            ? std::optional<std::string>(std::string(*container))
            : std::nullopt,
    };
    return true;
}

void InputRouter::set_clipboard(std::optional<std::string> text_value) {
    host_services_->set_clipboard_fallback(std::move(text_value));
}

std::optional<std::string_view> InputRouter::clipboard_text() const noexcept {
    return host_services_->clipboard_fallback();
}

} // namespace strata::ui
