#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/behavior/input.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"
#include "ui/presentation_geometry.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"
#include "ui/widget/editor_geometry.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {
[[nodiscard]] bool
pointer_geometry_generations_match(const DirtyGenerationSnapshot& left,
                                   const DirtyGenerationSnapshot& right) noexcept {
    return left.structure == right.structure && left.properties == right.properties &&
           left.layout == right.layout && left.text == right.text && left.style == right.style &&
           left.input == right.input && left.scale == right.scale &&
           left.animation == right.animation && left.editor == right.editor;
}

[[nodiscard]] bool contains(const Rect& bounds, const Point point) noexcept {
    return point.x >= bounds.x && point.y >= bounds.y && point.x <= bounds.right() &&
           point.y <= bounds.bottom();
}

[[nodiscard]] std::size_t text_boundary(const std::string_view text, std::size_t offset) noexcept {
    offset = std::min(offset, text.size());
    while (offset > 0U && offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
        --offset;
    }
    return offset;
}

[[nodiscard]] std::size_t previous_text_boundary(const std::string_view text,
                                                 const std::size_t offset) noexcept {
    std::size_t result = text_boundary(text, offset);
    if (result == 0U)
        return 0U;
    --result;
    while (result > 0U && (static_cast<unsigned char>(text[result]) & 0xC0U) == 0x80U) {
        --result;
    }
    return result;
}

[[nodiscard]] std::size_t next_text_boundary(const std::string_view text,
                                             const std::size_t offset) noexcept {
    std::size_t result = text_boundary(text, offset);
    if (result >= text.size())
        return text.size();
    ++result;
    while (result < text.size() && (static_cast<unsigned char>(text[result]) & 0xC0U) == 0x80U) {
        ++result;
    }
    return result;
}

[[nodiscard]] int text_word_class(const std::string_view text, const std::size_t offset) noexcept {
    if (offset >= text.size())
        return -1;
    const unsigned char value = static_cast<unsigned char>(text[offset]);
    if (value == '\n' || value == '\r')
        return 3;
    if (value == '\t' || value == ' ')
        return 2;
    if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value == '_' || value >= 0x80U) {
        return 1;
    }
    return 0;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> word_range(const std::string_view text,
                                                             const std::size_t offset) noexcept {
    if (text.empty())
        return {0U, 0U};
    const std::size_t anchor = text_boundary(text, offset);
    const std::size_t probe = anchor < text.size() ? anchor : previous_text_boundary(text, anchor);
    const int kind = text_word_class(text, probe);
    std::size_t start = probe;
    while (start > 0U) {
        const std::size_t previous = previous_text_boundary(text, start);
        if (text_word_class(text, previous) != kind)
            break;
        start = previous;
    }
    std::size_t end = next_text_boundary(text, probe);
    while (end < text.size() && text_word_class(text, end) == kind) {
        end = next_text_boundary(text, end);
    }
    return {start, end};
}

[[nodiscard]] std::size_t text_line_start(const std::string_view text,
                                          const std::size_t offset) noexcept {
    const std::size_t clamped = text_boundary(text, offset);
    const std::size_t newline =
        clamped == 0U ? std::string_view::npos : text.rfind('\n', clamped - 1U);
    return newline == std::string_view::npos ? 0U : newline + 1U;
}

[[nodiscard]] std::size_t text_line_end(const std::string_view text,
                                        const std::size_t offset) noexcept {
    const std::size_t newline = text.find('\n', text_boundary(text, offset));
    return newline == std::string_view::npos ? text.size() : newline;
}

} // namespace

InputDispatchContext::InputDispatchContext(
    InputRouter& router, RetainedNode& node, RetainedNode* const target,
    RetainedNode* const pointer_target, const InputEventPhase phase, const InputEventKind kind,
    InputDispatchState& state, const PointerInputEvent* const pointer,
    const ScrollInputEvent* const scroll, const KeyInputEvent* const key,
    const TextInputEvent* const text, const ImePreeditInputEvent* const ime_preedit) noexcept
    : router_(router), node_(node), target_(target), pointer_target_(pointer_target), phase_(phase),
      kind_(kind), state_(state), pointer_(pointer), scroll_(scroll), key_(key), text_(text),
      ime_preedit_(ime_preedit) {}

RetainedNode& InputDispatchContext::node() noexcept {
    return node_;
}
const RetainedNode& InputDispatchContext::node() const noexcept {
    return node_;
}
RetainedNode* InputDispatchContext::target() const noexcept {
    return target_;
}
RetainedNode* InputDispatchContext::pointer_target() const noexcept {
    return pointer_target_;
}
InputEventPhase InputDispatchContext::phase() const noexcept {
    return phase_;
}
InputEventKind InputDispatchContext::kind() const noexcept {
    return kind_;
}
const PointerInputEvent* InputDispatchContext::pointer() const noexcept {
    return pointer_;
}
const ScrollInputEvent* InputDispatchContext::scroll() const noexcept {
    return scroll_;
}
const KeyInputEvent* InputDispatchContext::key() const noexcept {
    return key_;
}
const TextInputEvent* InputDispatchContext::text() const noexcept {
    return text_;
}
const ImePreeditInputEvent* InputDispatchContext::ime_preedit() const noexcept {
    return ime_preedit_;
}
bool InputDispatchContext::consumed() const noexcept {
    return state_.consumed;
}
bool InputDispatchContext::propagation_stopped() const noexcept {
    return state_.propagation_stopped;
}

std::optional<Point> InputDispatchContext::press_origin() const noexcept {
    if (pointer_ == nullptr)
        return std::nullopt;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    return found != router_.pressed_pointer_targets_.end() ? std::optional(found->second.position)
                                                           : std::nullopt;
}

std::optional<Point> InputDispatchContext::press_last_position() const noexcept {
    if (pointer_ == nullptr)
        return std::nullopt;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    return found != router_.pressed_pointer_targets_.end()
               ? std::optional(found->second.last_position)
               : std::nullopt;
}

bool InputDispatchContext::press_moved_beyond_slop() const noexcept {
    if (pointer_ == nullptr)
        return false;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    return found != router_.pressed_pointer_targets_.end() && found->second.moved_beyond_slop;
}

bool InputDispatchContext::long_press_emitted() const noexcept {
    if (pointer_ == nullptr)
        return false;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    return found != router_.pressed_pointer_targets_.end() && found->second.long_press_emitted;
}

GestureClaimState InputDispatchContext::gesture_claim_state() const noexcept {
    if (pointer_ == nullptr)
        return GestureClaimState::unclaimed;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    return found != router_.pressed_pointer_targets_.end() ? found->second.gesture
                                                           : GestureClaimState::unclaimed;
}

std::optional<std::uint64_t> InputDispatchContext::gesture_claim_owner() const noexcept {
    if (pointer_ == nullptr)
        return std::nullopt;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    return found != router_.pressed_pointer_targets_.end() ? found->second.gesture_owner
                                                           : std::nullopt;
}

void InputDispatchContext::consume() noexcept {
    state_.consumed = true;
}

void InputDispatchContext::stop_propagation() noexcept {
    state_.consumed = true;
    state_.propagation_stopped = true;
}

bool InputDispatchContext::claim_gesture() noexcept {
    if (pointer_ == nullptr)
        return false;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    if (found == router_.pressed_pointer_targets_.end() ||
        found->second.gesture == GestureClaimState::cancelled ||
        (found->second.gesture_owner.has_value() &&
         found->second.gesture_owner != node_.identity())) {
        return false;
    }
    found->second.gesture = GestureClaimState::claimed;
    found->second.gesture_owner = node_.identity();
    state_.consumed = true;
    return true;
}

bool InputDispatchContext::cancel_gesture() noexcept {
    if (pointer_ == nullptr)
        return false;
    const auto found = router_.pressed_pointer_targets_.find(pointer_->pointer_id);
    if (found == router_.pressed_pointer_targets_.end() ||
        (found->second.gesture_owner.has_value() &&
         found->second.gesture_owner != node_.identity())) {
        return false;
    }
    found->second.gesture = GestureClaimState::cancelled;
    if (!found->second.gesture_owner.has_value()) {
        found->second.gesture_owner = node_.identity();
    }
    state_.consumed = true;
    return true;
}

void InputRouter::place_pointer_caret(RetainedNode& node, const PointerInputEvent& event,
                                      const bool extend_selection, InputOperationResult& result) {
    text_navigation_.erase(node.identity());
    if (static_text_selectable(node)) {
        const std::optional<std::string_view> text = static_text_value(node);
        if (!text.has_value())
            return;
        const std::optional<std::size_t> offset = resolve_text_offset(node, *text, event.position);
        if (!offset.has_value())
            return;

        std::size_t clicks = 1U;
        if (event.type == PointerEventType::press && !extend_selection) {
            const bool same_run = last_text_click_ == node.identity() &&
                                  frame_time_nanos_ >= last_text_click_nanos_ &&
                                  frame_time_nanos_ - last_text_click_nanos_ <=
                                      input_config_.multi_click_interval_nanos &&
                                  std::abs(event.position.x - last_text_click_position_.x) <=
                                      input_config_.multi_click_slop &&
                                  std::abs(event.position.y - last_text_click_position_.y) <=
                                      input_config_.multi_click_slop;
            text_click_count_ = same_run ? text_click_count_ % 3U + 1U : 1U;
            last_text_click_ = node.identity();
            last_text_click_position_ = event.position;
            last_text_click_nanos_ = frame_time_nanos_;
            clicks = text_click_count_;
        } else if (event.type == PointerEventType::press) {
            last_text_click_.reset();
            text_click_count_ = 0U;
        }

        const std::size_t focus = text_boundary(*text, *offset);
        if (clicks >= 3U) {
            set_static_text_selection(node, text_line_start(*text, focus),
                                      text_line_end(*text, focus), result);
        } else if (clicks == 2U) {
            const auto [start, end] = word_range(*text, focus);
            set_static_text_selection(node, start, end, result);
        } else {
            const auto existing = static_text_ranges_.find(node.identity());
            const std::size_t anchor = extend_selection && existing != static_text_ranges_.end()
                                           ? existing->second.anchor
                                           : focus;
            set_static_text_selection(node, anchor, focus, result);
            seed_pointer_text_navigation(node, *text, focus, event.position);
        }
        return;
    }
    const auto editor = editors_.find(node.identity());
    if (editor == editors_.end())
        return;

    const std::optional<std::size_t> offset =
        resolve_text_offset(node, editor->second.text(), event.position);
    if (!offset.has_value())
        return;

    std::size_t clicks = 1U;
    if (event.type == PointerEventType::press && !extend_selection) {
        const bool same_run = last_text_click_ == node.identity() &&
                              frame_time_nanos_ >= last_text_click_nanos_ &&
                              frame_time_nanos_ - last_text_click_nanos_ <=
                                  input_config_.multi_click_interval_nanos &&
                              std::abs(event.position.x - last_text_click_position_.x) <=
                                  input_config_.multi_click_slop &&
                              std::abs(event.position.y - last_text_click_position_.y) <=
                                  input_config_.multi_click_slop;
        text_click_count_ = same_run ? text_click_count_ % 3U + 1U : 1U;
        last_text_click_ = node.identity();
        last_text_click_position_ = event.position;
        last_text_click_nanos_ = frame_time_nanos_;
        clicks = text_click_count_;
    } else if (event.type == PointerEventType::press) {
        last_text_click_.reset();
        text_click_count_ = 0U;
    }

    const TextEditorMutation mutation = clicks >= 3U ? editor->second.select_line_at(*offset)
                                        : clicks == 2U
                                            ? editor->second.select_word_at(*offset)
                                            : editor->second.place_caret(*offset, extend_selection);
    record_editor_mutation(node, mutation, result);
    seed_pointer_text_navigation(node, editor->second.text(), editor->second.snapshot().caret,
                                 event.position);
}

std::optional<std::size_t> InputRouter::resolve_text_offset(const RetainedNode& node,
                                                            const std::string_view text,
                                                            Point position) const {
    const LayoutRecord* layout = layout_ != nullptr ? layout_->find(node.identity()) : nullptr;
    if (layout == nullptr)
        return std::nullopt;

    position = logical_pointer_position(node, position);
    if (static_text_node(node) && text_layout_resolver_) {
        const TextLayout visual = text_layout_resolver_(node, text, TextLayoutOptions{});
        const std::size_t utf16 =
            text_layout_hit_offset(visual, Point{
                                               position.x - layout->content_bounds.x,
                                               position.y - layout->content_bounds.y,
                                           });
        return utf8_byte_for_utf16_offset(text, utf16);
    }
    if (!text_offset_resolver_)
        return std::nullopt;

    return text_offset_resolver_(node, *layout, text, position);
}

Point InputRouter::logical_pointer_position(const RetainedNode& node, Point position) const {
    if (motion_ != nullptr) {
        MotionTransform transform;
        std::vector<const RetainedNode*> route;
        for (const RetainedNode* current = &node; current != nullptr; current = current->parent()) {
            route.push_back(current);
        }
        for (auto current = route.rbegin(); current != route.rend(); ++current) {
            const LayoutRecord* current_layout = layout_->find((*current)->identity());
            if (current_layout == nullptr)
                continue;
            transform = concatenate_presentation_transform(
                transform,
                local_presentation_transform(**current, *motion_, current_layout->bounds));
        }
        position = inverse_presentation_point(position, transform);
    }
    return position;
}

void InputRouter::seed_pointer_text_navigation(const RetainedNode& node,
                                               const std::string_view text, const std::size_t caret,
                                               const Point position) {
    const LayoutRecord* record = layout_ != nullptr ? layout_->find(node.identity()) : nullptr;
    if (record == nullptr || !text_layout_resolver_)
        return;
    const TextLayout visual = text_layout_resolver_(node, text, TextLayoutOptions{});
    if (visual.lines.empty())
        return;

    Point origin{record->content_bounds.x, record->content_bounds.y};
    if (!static_text_node(node)) {
        const std::vector<WidgetSubtarget> targets = subtargets(node.identity());
        const std::optional<Rect> viewport = editable_text_viewport(node, *record, targets);
        if (!viewport.has_value() || viewport->empty())
            return;
        const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
        const bool multiline = lifecycle != nullptr &&
                               lifecycle->input.text_edit_mode == WidgetTextEditMode::multi_line;
        origin = text_input_origin(*viewport, visual, multiline);
    }

    const Point logical = logical_pointer_position(node, position);
    const std::size_t pointer_line = text_layout_line_at_y(visual, logical.y - origin.y);
    const std::size_t utf16_caret = utf16_offset_for_utf8_byte(text, caret);
    const std::size_t line = text_layout_caret_line(visual, utf16_caret, pointer_line);
    text_navigation_.insert_or_assign(node.identity(),
                                      TextNavigationState{
                                          caret,
                                          line,
                                          shaped_caret_x(visual.shaped, line, utf16_caret),
                                      });
}

bool InputRouter::static_text_node(const RetainedNode& node) const noexcept {
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    return lifecycle != nullptr &&
           lifecycle->input.text_edit_mode == WidgetTextEditMode::static_text;
}

bool InputRouter::static_text_selectable(const RetainedNode& node) const noexcept {
    if (!static_text_node(node))
        return false;
    const runtime::Value* selectable = scalar_property(node, "selectable");
    if (selectable != nullptr && selectable->boolean() != nullptr) {
        return *selectable->boolean();
    }
    // Frozen Kotlin factories and DSL lowering both default selectable to true. The portable
    // registry keeps the property optional (and non-null when supplied), so absence retains that
    // frozen default; selectionContainer only bounds reading order and never overrides false.
    return true;
}

RetainedNode* InputRouter::selectable_static_text_owner(RetainedNode* hit) const noexcept {
    for (RetainedNode* current = hit; current != nullptr; current = current->parent()) {
        if (static_text_node(*current)) {
            return static_text_selectable(*current) ? current : nullptr;
        }
    }
    return nullptr;
}

std::optional<std::string_view>
InputRouter::static_text_value(const RetainedNode& node) const noexcept {
    const runtime::Value* value = scalar_property(node, "text");
    if (value == nullptr || value->string() == nullptr)
        value = node.retained_value("$text");
    return value != nullptr && value->string() != nullptr
               ? std::optional<std::string_view>(*value->string())
               : std::nullopt;
}

std::optional<std::string_view>
InputRouter::static_text_container(const RetainedNode& node) const noexcept {
    const runtime::Value* value = scalar_property(node, "selectionContainer");
    return value != nullptr && value->string() != nullptr && !value->string()->empty()
               ? std::optional<std::string_view>(*value->string())
               : std::nullopt;
}

std::vector<RetainedNode*> InputRouter::static_text_nodes(const std::string_view container) const {
    std::vector<RetainedNode*> result;
    if (tree_ == nullptr || tree_->root() == nullptr || container.empty())
        return result;
    const auto visit = [this, container, &result](const auto& self, RetainedNode& node) -> void {
        const auto candidate = static_text_container(node);
        if (static_text_selectable(node) && candidate.has_value() && *candidate == container) {
            result.push_back(&node);
        }
        for (const auto& child : node.children())
            self(self, *child);
    };
    visit(visit, *tree_->root());
    return result;
}

void InputRouter::begin_static_text_selection(RetainedNode& node, InputOperationResult& result) {
    static_cast<void>(result);
    if (!static_text_selectable(node)) {
        static_text_selection_.reset();
        return;
    }
    const auto selected = static_text_ranges_.find(node.identity());
    if (selected == static_text_ranges_.end())
        return;
    const std::optional<std::string_view> container = static_text_container(node);
    static_text_selection_ = StaticTextSelectionSession{
        node.identity(),
        selected->second.focus,
        container.has_value() ? std::optional<std::string>(std::string(*container)) : std::nullopt,
    };
    std::vector<std::uint64_t> other_selections;
    other_selections.reserve(static_text_ranges_.size());
    for (const auto& [identity, range] : static_text_ranges_) {
        static_cast<void>(range);
        if (identity == node.identity())
            continue;
        other_selections.push_back(identity);
    }
    bool changed = false;
    for (const std::uint64_t identity : other_selections) {
        RetainedNode* candidate = tree_ != nullptr ? tree_->find_identity(identity) : nullptr;
        changed = static_text_ranges_.erase(identity) > 0U || changed;
        text_navigation_.erase(identity);
        if (candidate != nullptr) {
            static_cast<void>(tree_->mark(identity, DirtyReason::input));
        }
    }
    if (changed && frame_invalidator_)
        frame_invalidator_();
}

bool InputRouter::static_text_selection_owner_matches(const RetainedNode& node) const noexcept {
    if (!static_text_selection_.has_value())
        return false;
    const std::optional<std::string_view> container = static_text_container(node);
    if (container.has_value()) {
        return static_text_selection_->container.has_value() &&
               *static_text_selection_->container == *container;
    }
    return !static_text_selection_->container.has_value() &&
           static_text_selection_->anchor_identity == node.identity();
}

void InputRouter::transition_static_text_selection_owner(RetainedNode* const owner,
                                                         InputOperationResult& result) {
    static_cast<void>(result);
    const bool same_owner = owner != nullptr && static_text_selection_owner_matches(*owner);
    const std::optional<std::string_view> owner_container =
        owner != nullptr ? static_text_container(*owner) : std::nullopt;
    bool changed = false;
    for (auto range = static_text_ranges_.begin(); range != static_text_ranges_.end();) {
        RetainedNode* node = tree_ != nullptr ? tree_->find_identity(range->first) : nullptr;
        const bool belongs =
            owner != nullptr && node != nullptr &&
            (owner_container.has_value() ? static_text_container(*node) == owner_container
                                         : node->identity() == owner->identity());
        if (belongs) {
            ++range;
            continue;
        }
        if (node != nullptr)
            static_cast<void>(tree_->mark(node->identity(), DirtyReason::input));
        text_navigation_.erase(range->first);
        range = static_text_ranges_.erase(range);
        changed = true;
    }
    if (!same_owner)
        static_text_selection_.reset();
    if (changed && frame_invalidator_)
        frame_invalidator_();
}

void InputRouter::extend_static_text_selection(RetainedNode& candidate, const Point position,
                                               InputOperationResult& result) {
    static_cast<void>(result);
    if (!static_text_selection_.has_value() || !static_text_selectable(candidate) ||
        !static_text_selection_owner_matches(candidate)) {
        return;
    }
    const std::optional<std::string_view> focus_text = static_text_value(candidate);
    if (!focus_text.has_value())
        return;
    const std::optional<std::size_t> focus_offset =
        resolve_text_offset(candidate, *focus_text, position);
    if (!focus_offset.has_value())
        return;
    const std::vector<RetainedNode*> nodes =
        static_text_selection_->container.has_value()
            ? static_text_nodes(*static_text_selection_->container)
            : std::vector<RetainedNode*>{
                  tree_ != nullptr ? tree_->find_identity(static_text_selection_->anchor_identity)
                                   : nullptr,
              };
    if (nodes.empty() || nodes.front() == nullptr)
        return;
    const auto anchor =
        std::ranges::find(nodes, static_text_selection_->anchor_identity, &RetainedNode::identity);
    const auto focus = std::ranges::find(nodes, candidate.identity(), &RetainedNode::identity);
    if (anchor == nodes.end() || focus == nodes.end())
        return;
    const std::size_t anchor_index = static_cast<std::size_t>(anchor - nodes.begin());
    const std::size_t focus_index = static_cast<std::size_t>(focus - nodes.begin());
    const std::size_t first = std::min(anchor_index, focus_index);
    const std::size_t last = std::max(anchor_index, focus_index);
    bool changed = false;
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const std::optional<std::string_view> text = static_text_value(*nodes[index]);
        if (!text.has_value())
            continue;
        const std::size_t length = text->size();
        std::size_t start = 0U;
        std::size_t end = 0U;
        if (index >= first && index <= last) {
            if (anchor_index == focus_index) {
                start = static_text_selection_->anchor_offset;
                end = *focus_offset;
            } else if (index == anchor_index) {
                start = anchor_index < focus_index ? static_text_selection_->anchor_offset : 0U;
                end = anchor_index < focus_index ? length : static_text_selection_->anchor_offset;
            } else if (index == focus_index) {
                start = focus_index > anchor_index ? 0U : *focus_offset;
                end = focus_index > anchor_index ? *focus_offset : length;
            } else {
                end = length;
            }
        }
        const std::uint64_t identity = nodes[index]->identity();
        if (start == end && index != anchor_index && index != focus_index) {
            if (static_text_ranges_.erase(identity) > 0U) {
                changed = true;
                if (tree_ != nullptr) {
                    static_cast<void>(tree_->mark(identity, DirtyReason::input));
                }
            }
        } else {
            const StaticTextRange next{
                text_boundary(*text, start),
                text_boundary(*text, end),
            };
            const auto current = static_text_ranges_.find(identity);
            if (current == static_text_ranges_.end() || current->second.anchor != next.anchor ||
                current->second.focus != next.focus) {
                static_text_ranges_.insert_or_assign(identity, next);
                changed = true;
                if (tree_ != nullptr) {
                    static_cast<void>(tree_->mark(identity, DirtyReason::input));
                }
            }
        }
        text_navigation_.erase(identity);
    }
    if (changed && frame_invalidator_)
        frame_invalidator_();
}

void InputRouter::set_static_text_selection(RetainedNode& node, const std::size_t anchor,
                                            const std::size_t focus, InputOperationResult& result) {
    static_cast<void>(result);
    const std::optional<std::string_view> text = static_text_value(node);
    if (!text.has_value() || !static_text_selectable(node))
        return;
    const StaticTextRange next{text_boundary(*text, anchor), text_boundary(*text, focus)};
    const auto current = static_text_ranges_.find(node.identity());
    if (current != static_text_ranges_.end() && current->second.anchor == next.anchor &&
        current->second.focus == next.focus) {
        return;
    }
    static_text_ranges_.insert_or_assign(node.identity(), next);
    if (tree_ != nullptr)
        static_cast<void>(tree_->mark(node.identity(), DirtyReason::input));
    if (frame_invalidator_)
        frame_invalidator_();
}

bool InputRouter::move_static_text_selection(RetainedNode& node, const std::string_view key,
                                             const KeyModifiers modifiers,
                                             InputOperationResult& result) {
    if (!static_text_selectable(node))
        return false;
    if ((modifiers.control || modifiers.super_key) && key == "a") {
        static_cast<void>(select_all_static_text(node, result));
        return true;
    }
    if ((modifiers.control || modifiers.super_key) && key == "c") {
        static_cast<void>(copy_static_text_selection(node));
        return true;
    }
    if ((modifiers.control || modifiers.super_key) &&
        (key == "x" || key == "v" || key == "z" || key == "y")) {
        // Read-only selection owns editor-shaped shortcuts but never cuts, pastes, or mutates
        // history. This keeps focused static text from leaking those keys to app shortcuts.
        return true;
    }
    const bool navigation = key == "left" || key == "right" || key == "up" || key == "down" ||
                            key == "home" || key == "end";
    if (!navigation || modifiers.alt)
        return false;
    if (node.description().type == "RichText" && !rich_text_links(node).empty() &&
        !modifiers.shift) {
        return false;
    }
    const std::optional<std::string_view> text = static_text_value(node);
    if (!text.has_value())
        return false;
    const auto current = static_text_ranges_.find(node.identity());
    const std::size_t old_anchor =
        current != static_text_ranges_.end() ? current->second.anchor : 0U;
    const std::size_t old_focus = current != static_text_ranges_.end() ? current->second.focus : 0U;
    std::size_t next = old_focus;
    if (key == "left") {
        text_navigation_.erase(node.identity());
        next = previous_text_boundary(*text, old_focus);
    } else if (key == "right") {
        text_navigation_.erase(node.identity());
        next = next_text_boundary(*text, old_focus);
    } else if ((key == "home" || key == "end") && (modifiers.control || modifiers.super_key)) {
        text_navigation_.erase(node.identity());
        next = key == "home" ? 0U : text->size();
    } else {
        const std::optional<std::size_t> visual =
            visual_text_navigation_offset(node, *text, old_focus, key);
        if (!visual.has_value())
            return true;
        next = *visual;
    }
    if (!modifiers.shift) {
        bool cleared = false;
        for (auto range = static_text_ranges_.begin(); range != static_text_ranges_.end();) {
            if (range->first == node.identity()) {
                ++range;
                continue;
            }
            if (tree_ != nullptr) {
                static_cast<void>(tree_->mark(range->first, DirtyReason::input));
            }
            text_navigation_.erase(range->first);
            range = static_text_ranges_.erase(range);
            cleared = true;
        }
        if (cleared && frame_invalidator_)
            frame_invalidator_();
        const std::optional<std::string_view> container = static_text_container(node);
        static_text_selection_ = StaticTextSelectionSession{
            node.identity(),
            next,
            container.has_value() ? std::optional<std::string>(std::string(*container))
                                  : std::nullopt,
        };
    }
    set_static_text_selection(node, modifiers.shift ? old_anchor : next, next, result);
    return true;
}

void InputRouter::prepare_pointer_geometry() const {
    if (tree_ == nullptr || tree_->root() == nullptr || layout_ == nullptr) {
        pointer_hit_entries_.clear();
        projected_subtargets_.clear();
        detached_subtargets_.clear();
        pointer_geometry_ready_ = false;
        detached_subtargets_ready_ = false;
        return;
    }
    const DirtyGenerationSnapshot dirty = tree_->dirty_generations();
    const std::uint64_t notifications = notifications_.generation();
    if (pointer_geometry_ready_ && pointer_geometry_tree_ == tree_ &&
        pointer_geometry_layout_ == layout_ && pointer_geometry_motion_ == motion_ &&
        pointer_geometry_tree_generation_ == tree_->generation() &&
        pointer_geometry_layout_generation_ == layout_->generation &&
        pointer_geometry_notification_generation_ == notifications &&
        pointer_geometry_generations_match(pointer_geometry_dirty_generations_, dirty)) {
        return;
    }

    pointer_geometry_ready_ = false;
    detached_subtargets_ready_ = false;
    pointer_hit_entries_.clear();
    projected_subtargets_.clear();
    detached_subtargets_.clear();
    pointer_hit_entries_.reserve(layout_->records.size());
    std::vector<RetainedNode*> portals;
    const auto collect_portals = [this, &portals](const auto& self, RetainedNode& node,
                                                  const bool inside_portal) -> void {
        const LayoutRecord* record = layout_->find(node.identity());
        if (record == nullptr)
            return;
        const bool portal = record->kind == LayoutKind::portal;
        if (portal && !inside_portal)
            portals.push_back(&node);
        for (const auto& child : node.children()) {
            self(self, *child, inside_portal || portal);
        }
    };
    collect_portals(collect_portals, *tree_->root(), false);
    const auto visit = [this](const auto& self, RetainedNode& node, const bool visit_portals,
                              const MotionTransform inherited, const double inherited_opacity,
                              const std::optional<Rect> traversal_clip) -> void {
        if (node.lifecycle() == RetainedLifecycle::exiting)
            return;
        const LayoutRecord* record = layout_->find(node.identity());
        if (record == nullptr)
            return;
        if (!visit_portals && record->kind == LayoutKind::portal)
            return;
        const double opacity = inherited_opacity * local_presentation_opacity(node, motion_);
        if (opacity <= 1.0e-3)
            return;
        const MotionTransform transform =
            motion_ != nullptr
                ? concatenate_presentation_transform(
                      inherited, local_presentation_transform(node, *motion_, record->bounds))
                : inherited;
        std::optional<Rect> child_clip = traversal_clip;
        if (record->clip.has_value()) {
            const Rect transformed = transform_presentation_bounds(*record->clip, inherited);
            child_clip = child_clip.has_value()
                             ? std::optional(child_clip->clip_intersection(transformed))
                             : std::optional(transformed);
        }
        std::vector<RetainedNode*> children;
        children.reserve(node.children().size());
        for (const auto& child : node.children())
            children.push_back(child.get());
        std::ranges::stable_sort(
            children, [this](const RetainedNode* left, const RetainedNode* right) {
                const LayoutRecord* left_layout = layout_->find(left->identity());
                const LayoutRecord* right_layout = layout_->find(right->identity());
                return (left_layout != nullptr ? left_layout->z_index : 0) <
                       (right_layout != nullptr ? right_layout->z_index : 0);
            });
        for (auto child = children.rbegin(); child != children.rend(); ++child) {
            self(self, **child, visit_portals, transform, opacity, child_clip);
        }
        const Rect bounds =
            hit_bounds_resolver_ ? hit_bounds_resolver_(node, *record) : record->hit_bounds;
        pointer_hit_entries_.push_back(PointerHitEntry{
            &node,
            transform_presentation_bounds(bounds, transform),
            traversal_clip,
            transform,
        });
    };
    // Detached portal roots occupy the final render plane. Visit them first in reverse paint
    // order so hit testing observes the same topmost-first order as rendering.
    for (auto portal = portals.rbegin(); portal != portals.rend(); ++portal) {
        visit(visit, **portal, true, MotionTransform{}, 1.0, std::nullopt);
    }
    visit(visit, *tree_->root(), false, MotionTransform{}, 1.0, std::nullopt);
    pointer_geometry_tree_ = tree_;
    pointer_geometry_layout_ = layout_;
    pointer_geometry_motion_ = motion_;
    pointer_geometry_tree_generation_ = tree_->generation();
    pointer_geometry_layout_generation_ = layout_->generation;
    pointer_geometry_notification_generation_ = notifications;
    pointer_geometry_dirty_generations_ = dirty;
    pointer_geometry_ready_ = true;
    ++pointer_geometry_rebuild_count_;
}

const std::vector<WidgetSubtarget>&
InputRouter::projected_subtargets(const RetainedNode& node) const {
    prepare_pointer_geometry();
    const auto found = projected_subtargets_.find(node.identity());
    if (found != projected_subtargets_.end())
        return found->second;
    const WidgetLifecycle* const lifecycle = widgets_.find(node.description().type);
    const bool extension_subtargets =
        lifecycle != nullptr && lifecycle->inspection.subtargets != nullptr;
    if (inside_native_presentation(node) || !node_participates(node) ||
        (!extension_subtargets && !widget_projects_subtargets(node.description().type))) {
        return projected_subtargets_.emplace(node.identity(), std::vector<WidgetSubtarget>{})
            .first->second;
    }
    std::vector<WidgetSubtarget> targets;
    if (extension_subtargets) {
        const LayoutRecord* const record = layout_->find(node.identity());
        if (record != nullptr)
            targets = lifecycle->inspection.subtargets(node, *record);
    } else {
        const std::string* query = edited_text(node.identity());
        targets = widget_subtargets(
            node, *layout_, commands_, text_width_resolver_, text_layout_resolver_, &notifications_,
            query != nullptr ? std::string_view(*query) : std::string_view{});
    }
    return projected_subtargets_.emplace(node.identity(), std::move(targets)).first->second;
}

void InputRouter::prepare_detached_subtargets() const {
    prepare_pointer_geometry();
    if (!pointer_geometry_ready_ || detached_subtargets_ready_)
        return;
    const std::vector<DetachedOverlayRoot> overlays =
        detached_overlay_roots(*tree_, *layout_, [this](const RetainedNode& node) {
            if (!widget_projects_detached_subtargets(node.description().type))
                return false;
            return std::ranges::any_of(projected_subtargets(node), &WidgetSubtarget::detached);
        });
    for (auto root = overlays.rbegin(); root != overlays.rend(); ++root) {
        const std::vector<WidgetSubtarget>& projected = projected_subtargets(*root->node);
        std::vector<const WidgetSubtarget*> detached;
        detached.reserve(projected.size());
        for (const WidgetSubtarget& target : projected) {
            if (target.detached)
                detached.push_back(&target);
        }
        std::ranges::stable_sort(detached, {},
                                 [](const WidgetSubtarget* target) { return target->z_index; });
        for (auto target = detached.rbegin(); target != detached.rend(); ++target) {
            WidgetSubtarget presented = **target;
            if (presented.presentation_identity.has_value()) {
                const auto entry = std::ranges::find(
                    pointer_hit_entries_, *presented.presentation_identity,
                    [](const PointerHitEntry& candidate) {
                        return candidate.node != nullptr ? candidate.node->identity() : 0U;
                    });
                if (entry == pointer_hit_entries_.end())
                    continue;
                presented.bounds =
                    transform_presentation_bounds(presented.bounds, entry->transform);
                presented.presentation_clip = entry->traversal_clip;
            }
            detached_subtargets_.push_back(std::move(presented));
        }
    }
    detached_subtargets_ready_ = true;
}

RetainedNode* InputRouter::hit_test(const Point position) const noexcept {
    prepare_pointer_geometry();
    for (const PointerHitEntry& entry : pointer_hit_entries_) {
        if ((!entry.traversal_clip.has_value() || contains(*entry.traversal_clip, position)) &&
            contains(entry.bounds, position)) {
            return entry.node;
        }
    }
    return nullptr;
}

const RetainedNode* InputRouter::inspection_target(const Point position) const noexcept {
    return hit_test(position);
}

std::vector<WidgetSubtarget> InputRouter::subtargets(const std::uint64_t identity) const {
    if (tree_ == nullptr || layout_ == nullptr)
        return {};
    const RetainedNode* node = tree_->find_identity(identity);
    if (node == nullptr || inside_native_presentation(*node) || !node_participates(*node)) {
        return {};
    }
    const WidgetLifecycle* const lifecycle = widgets_.find(node->description().type);
    if (lifecycle != nullptr && lifecycle->inspection.subtargets != nullptr) {
        const LayoutRecord* const record = layout_->find(identity);
        return record != nullptr ? lifecycle->inspection.subtargets(*node, *record)
                                 : std::vector<WidgetSubtarget>{};
    }
    const std::string* query = edited_text(identity);
    return widget_subtargets(*node, *layout_, commands_, text_width_resolver_,
                             text_layout_resolver_, &notifications_,
                             query != nullptr ? std::string_view(*query) : std::string_view{});
}

bool InputRouter::subtarget_hovered(const std::uint64_t identity,
                                    const std::string_view id) const noexcept {
    return hovered_subtarget_.has_value() && hovered_subtarget_->first == identity &&
           hovered_subtarget_->second == id;
}

bool InputRouter::subtarget_active(const std::uint64_t identity,
                                   const std::string_view id) const noexcept {
    return active_subtarget_.has_value() && active_subtarget_->first == identity &&
           active_subtarget_->second == id;
}

std::optional<WidgetSubtarget>
InputRouter::hit_subtarget(const Point position, const RetainedNode* const ordinary_target) const {
    if (tree_ == nullptr || tree_->root() == nullptr || layout_ == nullptr)
        return std::nullopt;
    prepare_detached_subtargets();
    for (const WidgetSubtarget& target : detached_subtargets_) {
        if ((!target.presentation_clip.has_value() ||
             contains(*target.presentation_clip, position)) &&
            contains(target.bounds, position)) {
            return target;
        }
    }

    std::optional<WidgetSubtarget> best;
    for (const RetainedNode* current = ordinary_target; current != nullptr;
         current = current->parent()) {
        if (inside_native_presentation(*current))
            continue;
        if (!node_participates(*current))
            continue;
        const std::vector<WidgetSubtarget>& projected = projected_subtargets(*current);
        for (auto candidate = projected.rbegin(); candidate != projected.rend(); ++candidate) {
            if (candidate->detached || !contains(candidate->bounds, position))
                continue;
            if (!best.has_value() || candidate->z_index > best->z_index) {
                best = *candidate;
            }
        }
    }
    return best;
}

void InputRouter::set_hovered_subtarget(const std::optional<WidgetSubtarget>& target) {
    const std::optional<std::pair<std::uint64_t, std::string>> next =
        target.has_value() ? std::optional(std::pair(target->owner_identity, target->id))
                           : std::nullopt;
    const std::optional<std::uint64_t> next_notification =
        target.has_value() ? target->notification_id : std::nullopt;
    if (next == hovered_subtarget_ && next_notification == hovered_notification_id_)
        return;
    if (hovered_notification_id_ != next_notification) {
        if (hovered_notification_id_.has_value()) {
            static_cast<void>(notifications_.pause(*hovered_notification_id_, false));
        }
        if (next_notification.has_value()) {
            static_cast<void>(notifications_.pause(*next_notification, true));
        }
        hovered_notification_id_ = next_notification;
    }
    if (tree_ != nullptr && hovered_subtarget_.has_value()) {
        static_cast<void>(tree_->mark(hovered_subtarget_->first, DirtyReason::paint));
    }
    hovered_subtarget_ = next;
    if (tree_ != nullptr && hovered_subtarget_.has_value()) {
        static_cast<void>(tree_->mark(hovered_subtarget_->first, DirtyReason::paint));
    }
    if (frame_invalidator_)
        frame_invalidator_();
}

RetainedNode* InputRouter::interactive_ancestor(RetainedNode* node) const noexcept {
    for (RetainedNode* current = node; current != nullptr; current = current->parent()) {
        if (inside_native_presentation(*current))
            continue;
        const WidgetLifecycle* lifecycle = widgets_.find(current->description().type);
        if (lifecycle != nullptr &&
            (lifecycle->input.event != nullptr || lifecycle->input.pointer != nullptr ||
             lifecycle->input.click != nullptr || focusable(*current))) {
            // Text/RichText share registered hooks, but an explicitly non-selectable Text is
            // input-empty in the frozen contract. RichText links remain interactive via their
            // independently computed focus capability.
            if (lifecycle->input.text_edit_mode != WidgetTextEditMode::static_text ||
                focusable(*current)) {
                return current;
            }
        }
        for (const DescriptionBehavior& attachment : current->description().behaviors) {
            const BehaviorLifecycle* behavior_lifecycle =
                attachment.enabled ? behaviors_.find(attachment.id) : nullptr;
            if (behavior_lifecycle != nullptr && behavior_lifecycle->input.accepts_pointer &&
                !behavior_lifecycle->input.disabled) {
                return current;
            }
        }
    }
    return nullptr;
}

InputDispatchState InputRouter::route_event(
    RetainedNode* const target, RetainedNode* const pointer_target, const InputEventKind kind,
    const PointerInputEvent* const pointer, const ScrollInputEvent* const scroll,
    const KeyInputEvent* const key, const TextInputEvent* const text,
    const ImePreeditInputEvent* const ime_preedit, InputOperationResult& result) {
    InputDispatchState state;
    if (target == nullptr)
        return state;
    std::vector<RetainedNode*> route;
    for (RetainedNode* current = target; current != nullptr; current = current->parent()) {
        route.push_back(current);
    }
    const auto dispatch = [this, &result, target, pointer_target, kind, pointer, scroll, key, text,
                           ime_preedit, &state](RetainedNode& node, const InputEventPhase phase) {
        if (state.dispatches >= input_config_.max_dispatches_per_event) {
            state.consumed = true;
            state.propagation_stopped = true;
            return;
        }
        ++state.dispatches;
        ++dispatch_count_;
        InputDispatchContext context(*this, node, target, pointer_target, phase, kind, state,
                                     pointer, scroll, key, text, ime_preedit);
        for (const DescriptionBehavior& attachment : node.description().behaviors) {
            const BehaviorLifecycle* lifecycle =
                attachment.enabled ? behaviors_.find(attachment.id) : nullptr;
            if (lifecycle == nullptr || lifecycle->input.disabled)
                continue;
            const bool has_typed_hook = pointer != nullptr ? lifecycle->input.pointer != nullptr
                                        : key != nullptr && key->type != KeyEventType::release
                                            ? lifecycle->input.key != nullptr
                                            : false;
            if (lifecycle->input.event == nullptr && !has_typed_hook)
                continue;
            BehaviorInputScope scope(context, attachment, result);
            if (lifecycle->input.event != nullptr) {
                if (lifecycle->input.event(scope))
                    context.stop_propagation();
            }
            if (context.propagation_stopped())
                return;
            if (pointer != nullptr && lifecycle->input.pointer != nullptr) {
                if (lifecycle->input.pointer(scope))
                    context.stop_propagation();
            } else if (key != nullptr && key->type != KeyEventType::release &&
                       lifecycle->input.key != nullptr) {
                if (lifecycle->input.key(scope))
                    context.stop_propagation();
            }
            if (context.propagation_stopped())
                return;
        }
        const WidgetLifecycle* widget = widgets_.find(node.description().type);
        if (widget == nullptr || widget->input.event == nullptr)
            return;
        WidgetInputScope scope(
            *this, node, result, key != nullptr ? std::string_view(key->key) : std::string_view{},
            pointer != nullptr  ? pointer->modifiers
            : scroll != nullptr ? scroll->modifiers
            : key != nullptr    ? key->modifiers
                                : KeyModifiers{},
            pointer, pointer_target, 0U, routed_subtarget_,
            text != nullptr ? std::string_view(text->text) : std::string_view{}, &context);
        if (widget->input.event(scope))
            context.stop_propagation();
    };
    for (auto current = route.rbegin(); current != route.rend(); ++current) {
        if (*current == target)
            break;
        dispatch(**current, InputEventPhase::capture);
        if (state.propagation_stopped)
            return state;
    }
    dispatch(*target, InputEventPhase::target);
    if (state.propagation_stopped)
        return state;
    for (RetainedNode* current = target->parent(); current != nullptr;
         current = current->parent()) {
        dispatch(*current, InputEventPhase::bubble);
        if (state.propagation_stopped)
            return state;
    }
    return state;
}

InputDispatchState InputRouter::route_pointer_event(RetainedNode* const target,
                                                    RetainedNode* const pointer_target,
                                                    const PointerInputEvent& event,
                                                    InputOperationResult& result) {
    InputEventKind kind = InputEventKind::pointer_move;
    switch (event.type) {
    case PointerEventType::press:
        kind = InputEventKind::pointer_press;
        break;
    case PointerEventType::release:
        kind = InputEventKind::pointer_release;
        break;
    case PointerEventType::cancel:
        kind = InputEventKind::pointer_cancel;
        break;
    case PointerEventType::move: {
        const auto press = pressed_pointer_targets_.find(event.pointer_id);
        kind = press != pressed_pointer_targets_.end() && press->second.moved_beyond_slop
                   ? InputEventKind::pointer_drag
                   : InputEventKind::pointer_move;
        break;
    }
    }
    return route_event(target, pointer_target, kind, &event, nullptr, nullptr, nullptr, nullptr,
                       result);
}

InputDispatchState InputRouter::route_key_event(RetainedNode* const target,
                                                const KeyInputEvent& event,
                                                InputOperationResult& result) {
    return route_event(target, nullptr, InputEventKind::key, nullptr, nullptr, &event, nullptr,
                       nullptr, result);
}

bool InputRouter::route_widget_pointer(RetainedNode* target, RetainedNode* const pointer_target,
                                       const PointerInputEvent& event,
                                       InputDispatchState& dispatch_state,
                                       InputOperationResult& result) {
    for (RetainedNode* current = target; current != nullptr; current = current->parent()) {
        const WidgetLifecycle* lifecycle = widgets_.find(current->description().type);
        if (lifecycle == nullptr || lifecycle->input.pointer == nullptr)
            continue;
        InputDispatchContext context(
            *this, *current, target, pointer_target,
            current == target ? InputEventPhase::target : InputEventPhase::bubble,
            event.type == PointerEventType::press     ? InputEventKind::pointer_press
            : event.type == PointerEventType::release ? InputEventKind::pointer_release
            : event.type == PointerEventType::cancel  ? InputEventKind::pointer_cancel
                                                      : InputEventKind::pointer_move,
            dispatch_state, &event);
        WidgetInputScope scope(*this, *current, result, {}, event.modifiers, &event, pointer_target,
                               0U, routed_subtarget_, {}, &context);
        if (lifecycle->input.pointer(scope)) {
            context.stop_propagation();
            return true;
        }
        if (context.propagation_stopped())
            return true;
    }
    return false;
}

void InputRouter::route_active_lifecycle_hooks(const bool after_layout,
                                               InputOperationResult& result) {
    if (tree_ == nullptr)
        return;
    std::set<std::uint64_t> visited;
    const auto route = [this, after_layout, &result, &visited](const std::uint64_t identity) {
        for (RetainedNode* current = tree_->find_identity(identity); current != nullptr;
             current = current->parent()) {
            if (!visited.insert(current->identity()).second)
                continue;
            const WidgetLifecycle* lifecycle = widgets_.find(current->description().type);
            const WidgetInputHook* hook = lifecycle == nullptr ? nullptr
                                          : after_layout       ? &lifecycle->input.after_layout
                                                               : &lifecycle->input.advance;
            if (hook != nullptr && *hook != nullptr) {
                WidgetInputScope scope(*this, *current, result, {}, {});
                static_cast<void>((*hook)(scope));
            }
            for (const DescriptionBehavior& attachment : current->description().behaviors) {
                const BehaviorLifecycle* behavior =
                    attachment.enabled ? behaviors_.find(attachment.id) : nullptr;
                const BehaviorInputHook* behavior_hook = behavior == nullptr ? nullptr
                                                         : after_layout
                                                             ? &behavior->input.after_layout
                                                             : &behavior->input.advance;
                if (behavior_hook == nullptr || *behavior_hook == nullptr)
                    continue;
                BehaviorInputScope scope(*this, *current, attachment,
                                         after_layout ? BehaviorInputEventPhase::after_layout
                                                      : BehaviorInputEventPhase::advance,
                                         result);
                static_cast<void>((*behavior_hook)(scope));
            }
        }
    };
    for (const auto& [pointer_id, press] : pressed_pointer_targets_) {
        static_cast<void>(pointer_id);
        route(press.identity);
    }
    for (const auto& [pointer_id, session] : drag_sessions_) {
        static_cast<void>(pointer_id);
        if (session.active && session.target_identity.has_value()) {
            route(*session.target_identity);
        }
    }
}

InputOperationResult InputRouter::pointer(const PointerInputEvent event) {
    InputOperationResult result;
    result.injected_events = 1U;
    result.processed_events = 1U;
    if (tree_ == nullptr || layout_ == nullptr)
        return result;
    // A press changes modality even when it lands on the already-focused control or is consumed.
    // Motion alone must not erase a keyboard user's location indicator.
    if (event.type == PointerEventType::press)
        set_focus_visibility(false);
    RetainedNode* hover_target = widget_native_input_owner(hit_test(event.position));
    RetainedNode* const scrollbar_hit_target = hover_target;
    routed_subtarget_ = hit_subtarget(event.position, hover_target);
    if (routed_subtarget_.has_value()) {
        RetainedNode* owner = tree_->find_identity(routed_subtarget_->owner_identity);
        if (routed_subtarget_->kind == WidgetSubtargetKind::scrim && owner != nullptr &&
            hover_target != nullptr && hover_target != owner &&
            descendant_of(*hover_target, *owner)) {
            routed_subtarget_.reset();
        } else if (owner != nullptr) {
            hover_target = owner;
        }
    }
    RetainedNode* const static_text_owner =
        event.type == PointerEventType::press && event.button == 0
            ? selectable_static_text_owner(hover_target)
            : nullptr;
    if (event.type == PointerEventType::press && event.button == 0) {
        // Selection ownership changes before modal/interactive routing can consume or early-return
        // this press. Background, controls, opt-out text, and link-only RichText all pass null.
        transition_static_text_selection_owner(static_text_owner, result);
    }
    set_hovered_subtarget(routed_subtarget_);
    RetainedNode* target = interactive_ancestor(hover_target);
    if (RetainedNode* modal = active_modal(); modal != nullptr) {
        const bool routed_above_modal =
            routed_subtarget_.has_value() && routed_subtarget_->detached &&
            routed_subtarget_->owner_identity != modal->identity() &&
            routed_subtarget_->z_index > detached_overlay_z_index(*modal);
        const RetainedNode* focused =
            focused_.has_value() ? tree_->find_identity(*focused_) : nullptr;
        if (focused == nullptr || !descendant_of(*focused, *modal)) {
            focus(*modal, "programmatic", result);
        }
        if (!routed_above_modal && (target == nullptr || !descendant_of(*target, *modal))) {
            target = modal;
        }
        if (!routed_above_modal &&
            (hover_target == nullptr || !descendant_of(*hover_target, *modal))) {
            hover_target = modal;
        }
    }

    auto pressed = pressed_pointer_targets_.find(event.pointer_id);
    if (pressed != pressed_pointer_targets_.end()) {
        pressed->second.last_position = event.position;
        if (event.type == PointerEventType::move && !pressed->second.moved_beyond_slop) {
            const double delta_x = event.position.x - pressed->second.position.x;
            const double delta_y = event.position.y - pressed->second.position.y;
            const double slop = input_config_.pointer_drag_slop;
            if (event.coalesced_moved_beyond_slop ||
                delta_x * delta_x + delta_y * delta_y > slop * slop) {
                pressed->second.moved_beyond_slop = true;
            }
        }
        if (event.type == PointerEventType::cancel) {
            pressed->second.gesture = GestureClaimState::cancelled;
        }
    }

    const auto finish_capture = [this, &event] {
        const auto captured = pressed_pointer_targets_.find(event.pointer_id);
        if (captured != pressed_pointer_targets_.end()) {
            static_cast<void>(tree_->mark(captured->second.identity, DirtyReason::input));
            pressed_pointer_targets_.erase(captured);
        }
        if (active_.has_value())
            static_cast<void>(tree_->mark(*active_, DirtyReason::input));
        active_.reset();
        active_subtarget_.reset();
    };

    if (event.type == PointerEventType::press) {
        hover_route(hover_target);
        dismiss_transient_popups(target, result);
        if (target == nullptr) {
            if (!route_scrollbar_pointer(event, result, scrollbar_hit_target)) {
                apply_pointer_focus_default(event, hover_target, nullptr, result);
            }
            return result;
        }
        pressed_pointer_targets_.insert_or_assign(
            event.pointer_id,
            PointerPress{
                .identity = target->identity(),
                .position = event.position,
                .button = event.button,
                .timestamp_nanos =
                    event.timestamp_nanos > 0 ? event.timestamp_nanos : frame_time_nanos_,
                .last_position = event.position,
                .subtarget_id =
                    routed_subtarget_.has_value() ? routed_subtarget_->id : std::string{},
            });
        active_ = target->identity();
        active_subtarget_ =
            routed_subtarget_.has_value()
                ? std::optional(std::pair(routed_subtarget_->owner_identity, routed_subtarget_->id))
                : std::nullopt;
        static_cast<void>(tree_->mark(target->identity(), DirtyReason::input));
        InputDispatchState dispatch = route_pointer_event(target, hover_target, event, result);
        if (dispatch.consumed)
            return result;
        if (route_scrollbar_pointer(event, result, scrollbar_hit_target))
            return result;
        apply_pointer_focus_default(event, hover_target, target, result);
        if (event.button == 0) {
            if (static_text_owner != nullptr) {
                if (event.modifiers.shift &&
                    static_text_selection_owner_matches(*static_text_owner)) {
                    extend_static_text_selection(*static_text_owner, event.position, result);
                } else {
                    place_pointer_caret(*static_text_owner, event, false, result);
                    begin_static_text_selection(*static_text_owner, result);
                }
            } else {
                place_pointer_caret(*target, event, event.modifiers.shift, result);
            }
        }
        static_cast<void>(route_widget_pointer(target, hover_target, event, dispatch, result));
        return result;
    }

    pressed = pressed_pointer_targets_.find(event.pointer_id);
    RetainedNode* dispatch_target = pressed != pressed_pointer_targets_.end()
                                        ? tree_->find_identity(pressed->second.identity)
                                        : target;
    hover_route(hover_target);
    InputDispatchState dispatch = route_pointer_event(dispatch_target, hover_target, event, result);
    pressed = pressed_pointer_targets_.find(event.pointer_id);
    const bool gesture_claimed = pressed != pressed_pointer_targets_.end() &&
                                 pressed->second.gesture == GestureClaimState::claimed;
    const std::optional<std::uint64_t> widget_claim_owner =
        gesture_claimed && pressed->second.widget_lifecycle_claim ? pressed->second.gesture_owner
                                                                  : std::nullopt;
    if (route_pointer_drag(event, result)) {
        if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
            finish_capture();
        }
        return result;
    }
    if (dispatch.consumed) {
        if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
            finish_capture();
        }
        return result;
    }
    if (widget_claim_owner.has_value()) {
        if (RetainedNode* owner = tree_->find_identity(*widget_claim_owner); owner != nullptr) {
            static_cast<void>(route_widget_pointer(owner, hover_target, event, dispatch, result));
        }
    }
    if (gesture_claimed) {
        if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
            finish_capture();
        }
        return result;
    }
    if (route_scrollbar_pointer(event, result, scrollbar_hit_target)) {
        if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
            finish_capture();
        }
        return result;
    }
    if (dispatch_target != nullptr &&
        route_widget_pointer(dispatch_target, hover_target, event, dispatch, result)) {
        if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
            finish_capture();
        }
        return result;
    }
    if (event.type == PointerEventType::move) {
        pressed = pressed_pointer_targets_.find(event.pointer_id);
        if (pressed != pressed_pointer_targets_.end()) {
            if (RetainedNode* owner = tree_->find_identity(pressed->second.identity);
                owner != nullptr) {
                if (hover_target != nullptr && static_text_selection_.has_value()) {
                    extend_static_text_selection(*hover_target, event.position, result);
                } else {
                    place_pointer_caret(*owner, event, true, result);
                }
            }
        }
        return result;
    }
    if (event.type == PointerEventType::cancel) {
        finish_capture();
        return result;
    }

    pressed = pressed_pointer_targets_.find(event.pointer_id);
    const std::optional<std::uint64_t> pressed_identity =
        pressed != pressed_pointer_targets_.end()
            ? std::optional<std::uint64_t>(pressed->second.identity)
            : std::nullopt;
    const bool click_suppressed =
        pressed != pressed_pointer_targets_.end() &&
        (pressed->second.moved_beyond_slop || pressed->second.long_press_emitted ||
         pressed->second.gesture != GestureClaimState::unclaimed);
    const bool subtarget_matches =
        pressed == pressed_pointer_targets_.end() ||
        pressed->second.subtarget_id ==
            (routed_subtarget_.has_value() ? routed_subtarget_->id : std::string{});
    finish_capture();
    if (pressed_identity.has_value()) {
        hover_route(tree_->find_identity(*pressed_identity));
    } else {
        hover_route(hover_target);
    }
    if (!click_suppressed && subtarget_matches && target != nullptr &&
        pressed_identity == target->identity()) {
        const std::uint64_t click_identity =
            hover_target != nullptr ? hover_target->identity() : target->identity();
        const std::string current_subtarget =
            routed_subtarget_.has_value() ? routed_subtarget_->id : std::string{};
        const bool same_click = last_widget_click_ == click_identity &&
                                last_widget_click_subtarget_ == current_subtarget &&
                                frame_time_nanos_ >= last_widget_click_nanos_ &&
                                frame_time_nanos_ - last_widget_click_nanos_ <=
                                    input_config_.multi_click_interval_nanos &&
                                std::abs(event.position.x - last_widget_click_position_.x) <=
                                    input_config_.multi_click_slop &&
                                std::abs(event.position.y - last_widget_click_position_.y) <=
                                    input_config_.multi_click_slop;
        widget_click_count_ = same_click ? widget_click_count_ % 3U + 1U : 1U;
        last_widget_click_ = click_identity;
        last_widget_click_subtarget_ = current_subtarget;
        last_widget_click_position_ = event.position;
        last_widget_click_nanos_ = frame_time_nanos_;
        activate(*target, result, &event, hover_target, widget_click_count_, routed_subtarget_);
    }
    return result;
}

} // namespace strata::ui
