#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/behavior/input.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"
#include "ui/path.hpp"
#include "ui/presentation_geometry.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"
#include "ui/widget/editor_geometry.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

/** Whether `point` is inside `outlines`, even-odd: a ring's hole is outside it. */
[[nodiscard]] bool inside_outlines(const std::vector<std::vector<Point>>& outlines,
                                   const Point point) noexcept {
    bool inside = false;
    for (const std::vector<Point>& outline : outlines) {
        if (outline.size() < 3U)
            continue;
        Point previous = outline.back();
        for (const Point current : outline) {
            if ((current.y > point.y) != (previous.y > point.y) &&
                point.x < (previous.x - current.x) * (point.y - current.y) /
                                  (previous.y - current.y) +
                              current.x)
                inside = !inside;
            previous = current;
        }
    }
    return inside;
}

/**
 * A shape over `bounds` as outlines: a `hitShape`, or (`clipped`) the area a `clipShape` cuts, as
 * clip-path cuts what a pointer hits on the web. None for a malformed shape or a clip only a
 * stroke draws.
 */
[[nodiscard]] std::vector<std::vector<Point>> shape_outlines(const RetainedNode& node,
                                                            const runtime::Value* value,
                                                            const bool clipped, const Rect bounds) {
    if (value == nullptr || bounds.width <= 0.0 || bounds.height <= 0.0)
        return {};
    std::optional<PathShape> shape;
    if (clipped && value->string() != nullptr) {
        // The named outlines, filling the box as the clip draws them: a regular polygon with a
        // corner up, or a star whose points sit half a sector either side of up and whose
        // notches reach half way in.
        const std::string& name = *value->string();
        if (name == "circle" || name == "ellipse") {
            shape = ellipse_shape(Point{0.5, 0.5}, 0.5, 0.5, std::nullopt);
        } else if (name == "polygon" || name == "hexagon" || name == "diamond" || name == "star") {
            double sides = name == "hexagon" ? 6.0 : name == "diamond" ? 4.0 : 5.0;
            if (const std::optional<double> authored = visual_number(node, "clipSides");
                authored.has_value())
                sides = std::clamp(std::round(*authored), 3.0, 64.0);
            const auto count = static_cast<std::size_t>(sides);
            const double sector = 2.0 * std::numbers::pi / sides;
            const bool star = name == "star";
            std::vector<Point> corners;
            for (std::size_t index = 0U; index < count; ++index) {
                const auto corner = [&corners](const double angle, const double radius) {
                    corners.push_back(Point{0.5 + 0.5 * radius * std::sin(angle),
                                            0.5 - 0.5 * radius * std::cos(angle)});
                };
                const double at = sector * static_cast<double>(index);
                if (star) {
                    corner(at - sector * 0.5, 1.0);
                    corner(at, 0.5);
                } else {
                    corner(at, 1.0);
                }
            }
            shape = PathShape{};
            shape->path = Path::polyline(corners, true);
        }
    } else if (value->object() != nullptr) {
        try {
            shape = path_shape_from_value(value, false);
        } catch (const std::invalid_argument&) {
            return {};
        }
    }
    if (!shape.has_value() || (clipped && shape->stroke.has_value() && !shape->fill.has_value()))
        return {};
    std::vector<std::vector<Point>> outlines;
    for (const PathContour& contour :
         flatten_path(shape->path, Size{bounds.width, bounds.height}, 0.25)) {
        std::vector<Point> outline;
        outline.reserve(contour.points.size());
        for (const Point point : contour.points)
            outline.push_back(Point{bounds.x + point.x * bounds.width,
                                    bounds.y + point.y * bounds.height});
        outlines.push_back(std::move(outline));
    }
    return outlines;
}

/** The area a node's `clipShape` cuts it to, over its layout bounds; none without one. */
[[nodiscard]] std::vector<std::vector<Point>> clip_outlines(const RetainedNode& node,
                                                           const Rect bounds) {
    return shape_outlines(node, visual_value(node, "clipShape"), true, bounds);
}

/**
 * Where a node itself is hit, over `bounds`: its `hitShape`, or without one its `clipShape`'s area;
 * none leaves the whole box hittable.
 */
[[nodiscard]] std::vector<std::vector<Point>> hit_outlines(const RetainedNode& node,
                                                          const Rect bounds) {
    const auto found = node.description().properties.find("hitShape");
    if (found != node.description().properties.end() && found->second.value() != nullptr)
        return shape_outlines(node, found->second.value(), false, bounds);
    return clip_outlines(node, bounds);
}

[[nodiscard]] bool
pointer_geometry_generations_match(const DirtyGenerationSnapshot& left,
                                   const DirtyGenerationSnapshot& right) noexcept {
    // The input lane only records hover and press changes, which move no hit region; the
    // subtargets widgets project from that state are refreshed on their own.
    return left.structure == right.structure && left.properties == right.properties &&
           left.layout == right.layout && left.text == right.text && left.style == right.style &&
           left.scale == right.scale && left.animation == right.animation &&
           left.editor == right.editor;
}

/** Whether `point` is inside a subtarget's polygon, as presented; a target without one is. */
[[nodiscard]] bool inside_subtarget_shape(const WidgetSubtarget& target, const Point point,
                                          const MotionTransform& transform) noexcept {
    if (target.shape.size() < 3U)
        return true;
    bool inside = false;
    Point previous = transform_presentation_point(target.shape.back(), transform);
    for (const Point vertex : target.shape) {
        const Point current = transform_presentation_point(vertex, transform);
        // Even-odd crossing count along a ray toward +x.
        if ((current.y > point.y) != (previous.y > point.y) &&
            point.x < (previous.x - current.x) * (point.y - current.y) /
                              (previous.y - current.y) +
                          current.x) {
            inside = !inside;
        }
        previous = current;
    }
    return inside;
}

[[nodiscard]] bool contains(const Rect& bounds, const Point point) noexcept {
    return point.x >= bounds.x && point.y >= bounds.y && point.x <= bounds.right() &&
           point.y <= bounds.bottom();
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
    if (motion_ != nullptr)
        position = inverse_presentation_point(position, presentation_transform(node));
    return position;
}

MotionTransform InputRouter::presentation_transform(const RetainedNode& node) const {
    if (tree_ == nullptr || layout_ == nullptr || motion_ == nullptr)
        return MotionTransform{};
    if (!preparing_pointer_geometry_)
        prepare_pointer_geometry();
    if (const auto found = presentation_transforms_.find(node.identity());
        found != presentation_transforms_.end()) {
        return found->second;
    }
    // The hit pass skips exiting subtrees; compose from the nearest node it placed.
    const MotionTransform inherited =
        node.parent() != nullptr ? presentation_transform(*node.parent()) : MotionTransform{};
    const LayoutRecord* record = layout_->find(node.identity());
    return record != nullptr
               ? concatenate_presentation_transform(
                     inherited, local_presentation_transform(node, *motion_, record->bounds))
               : inherited;
}

Rect InputRouter::presented_bounds(const RetainedNode& node, const Rect bounds) const {
    return motion_ != nullptr ? transform_presentation_bounds(bounds, presentation_transform(node))
                              : bounds;
}

void InputRouter::seed_pointer_text_navigation(const RetainedNode& node,
                                               const std::string_view text, const std::size_t caret,
                                               const Point position) {
    const LayoutRecord* record = layout_ != nullptr ? layout_->find(node.identity()) : nullptr;
    if (record == nullptr || !text_layout_resolver_)
        return;
    Point origin{record->content_bounds.x, record->content_bounds.y};
    std::optional<EditorView> view;
    if (!static_text_node(node)) {
        view = editor_view(node, text);
        if (!view.has_value())
            return;
        origin = editable_text_origin(view->viewport, view->layout, view->multiline,
                                      editor_scroll(node.identity()));
    }
    const TextLayout visual = view.has_value()
                                  ? std::move(view->layout)
                                  : text_layout_resolver_(node, text, TextLayoutOptions{});
    if (visual.lines.empty())
        return;

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
    // Labels are inert unless selection is explicitly requested. Editors own separate selection.
    return false;
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
        std::nullopt,
        std::nullopt,
    };
    if (const std::optional<StaticTextSource> located = static_text_source(node);
        located.has_value()) {
        static_text_selection_->source = StaticTextSourceSelection{
            located->holder->identity(),
            located->offset + selected->second.anchor,
            located->offset + selected->second.focus,
        };
    }
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
    if (static_text_selection_.has_value() && static_text_selection_->source.has_value()) {
        extend_sourced_static_text_selection(&candidate, position);
        return;
    }
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
    // Arrows over interactive static text (links) move between its regions, not the caret.
    if (const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
        lifecycle != nullptr && lifecycle->input.static_text_interactive != nullptr &&
        !modifiers.shift && lifecycle->input.static_text_interactive(node)) {
        return false;
    }
    if (const std::optional<StaticTextSource> located = static_text_source(node);
        located.has_value()) {
        return move_sourced_static_text_selection(node, *located, key, modifiers, result);
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
            std::nullopt,
            std::nullopt,
        };
    }
    set_static_text_selection(node, modifiers.shift ? old_anchor : next, next, result);
    return true;
}

void InputRouter::prepare_pointer_geometry() const {
    if (tree_ == nullptr || tree_->root() == nullptr || layout_ == nullptr) {
        pointer_hit_entries_.clear();
        pointer_hit_index_.clear();
        presentation_transforms_.clear();
        projected_subtargets_.clear();
        detached_subtargets_.clear();
        pointer_geometry_ready_ = false;
        detached_subtargets_ready_ = false;
        return;
    }
    const DirtyGenerationSnapshot dirty = tree_->dirty_generations();
    const std::uint64_t notifications = notifications_.generation();
    const std::uint64_t command_generation = commands_ != nullptr ? commands_->generation() : 0U;
    if (pointer_geometry_ready_ && pointer_geometry_tree_ == tree_ &&
        pointer_geometry_layout_ == layout_ && pointer_geometry_motion_ == motion_ &&
        pointer_geometry_tree_generation_ == tree_->generation() &&
        pointer_geometry_layout_generation_ == layout_->generation &&
        pointer_geometry_notification_generation_ == notifications &&
        pointer_geometry_command_generation_ == command_generation &&
        (pointer_geometry_generations_match(pointer_geometry_dirty_generations_, dirty) ||
         place_moved_anchors())) {
        pointer_geometry_dirty_generations_.animation = dirty.animation;
        if (pointer_geometry_dirty_generations_.input != dirty.input) {
            // Hover and press move no hit region, but widgets project subtargets from their
            // interaction state (a hovered Tooltip's bubble): only those are projected again.
            projected_subtargets_.clear();
            detached_subtargets_.clear();
            detached_subtargets_ready_ = false;
            pointer_geometry_dirty_generations_.input = dirty.input;
        }
        return;
    }

    pointer_geometry_ready_ = false;
    detached_subtargets_ready_ = false;
    // Hooks run while the geometry is rebuilt project subtargets afresh rather than through it.
    preparing_pointer_geometry_ = true;
    struct Prepared final {
        bool& preparing;
        ~Prepared() { preparing = false; }
    } prepared{preparing_pointer_geometry_};
    pointer_hit_entries_.clear();
    pointer_hit_index_.clear();
    presentation_transforms_.clear();
    pointer_anchors_.clear();
    pointer_placements_.clear();
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
    // Within an anchor, a transform and the clips around a node are kept relative to the anchor
    // as well as on the surface (see place_moved_anchors).
    struct Anchored final {
        std::int32_t anchor = -1;
        MotionTransform relative;
        std::optional<Rect> relative_clip;
    };
    using ClipChain = std::shared_ptr<const std::vector<ClipOutline>>;
    const auto visit = [this](const auto& self, RetainedNode& node, const bool visit_portals,
                              const MotionTransform inherited, const double inherited_opacity,
                              const std::optional<Rect> traversal_clip, const Anchored anchored,
                              const ClipChain& clips) -> void {
        if (node.lifecycle() == RetainedLifecycle::exiting)
            return;
        const LayoutRecord* record = layout_->find(node.identity());
        if (record == nullptr)
            return;
        if (!visit_portals && record->kind == LayoutKind::portal)
            return;
        // Every placed node keeps the transform it is presented with, hittable or not: pointer
        // inversion, IME placement, spatial navigation and injection read it from here.
        const MotionTransform local = motion_ != nullptr
                                          ? local_presentation_transform(node, *motion_, record->bounds)
                                          : MotionTransform{};
        const MotionTransform transform = concatenate_presentation_transform(inherited, local);
        presentation_transforms_.insert_or_assign(node.identity(), transform);
        const double opacity = inherited_opacity * local_presentation_opacity(node, motion_);
        // A presentation group's root is an anchor: what is within it is kept relative to it.
        const bool anchor = motion_ != nullptr &&
                            (local_presentation_tone(node, *motion_).has_value() ||
                             motion_presentation_group(node, *motion_) ||
                             presentation_in_depth(node, *motion_) ||
                             parent_content_view(node).has_value());
        Anchored here{anchored.anchor,
                      concatenate_presentation_transform(anchored.relative, local),
                      anchored.relative_clip};
        if (anchor) {
            pointer_anchors_.push_back(PointerAnchor{node.identity(), anchored.anchor,
                                                     anchored.relative, local,
                                                     anchored.relative_clip, std::nullopt,
                                                     transform, traversal_clip, traversal_clip,
                                                     inherited_opacity, opacity > 1.0e-3});
            here = Anchored{static_cast<std::int32_t>(pointer_anchors_.size() - 1U), {}, {}};
        }
        pointer_placements_.push_back(PointerPlacement{node.identity(), here.anchor, here.relative});
        if (opacity <= 1.0e-3)
            return;
        std::optional<Rect> child_clip = traversal_clip;
        // The clips this node puts around its descendants, within its anchor: an anchor's own
        // clip is placed by what it is presented under, so it belongs to its parent's space.
        std::optional<Rect> child_relative_clip = anchor ? std::nullopt : here.relative_clip;
        const auto relative_clip = [&](const Rect clip) {
            const Rect placed = transform_presentation_bounds(clip, anchored.relative);
            if (anchor) {
                std::optional<Rect>& own = pointer_anchors_.back().own_clip;
                own = own.has_value() ? std::optional(own->clip_intersection(placed))
                                      : std::optional(placed);
                pointer_anchors_.back().world_inner_clip = child_clip;
                return;
            }
            child_relative_clip = child_relative_clip.has_value()
                                      ? std::optional(child_relative_clip->clip_intersection(placed))
                                      : std::optional(placed);
        };
        const auto clip_children = [&child_clip, &inherited, &relative_clip](const Rect clip) {
            const Rect transformed = transform_presentation_bounds(clip, inherited);
            child_clip = child_clip.has_value()
                             ? std::optional(child_clip->clip_intersection(transformed))
                             : std::optional(transformed);
            relative_clip(clip);
        };
        if (!transform.axis_aligned() && record->local_clip.has_value()) {
            // A rotated node's clip turns with it, as it is drawn; descendants are hit within the
            // box around the turned clip, and exactly within their own turned shapes.
            const Rect turned = transform_presentation_bounds(*record->local_clip, transform);
            child_clip = child_clip.has_value()
                             ? std::optional(child_clip->clip_intersection(turned))
                             : std::optional(turned);
            // Within the anchor, as for an anchor itself, which moving (turned) is never placed
            // again (place_moved_anchors needs an axis-aligned anchor).
            const Rect relative = transform_presentation_bounds(*record->local_clip, here.relative);
            child_relative_clip =
                child_relative_clip.has_value()
                    ? std::optional(child_relative_clip->clip_intersection(relative))
                    : std::optional(relative);
        } else if (record->local_clip.has_value()) {
            // Only the node's own clip: `record->clip` already holds its ancestors' clips in
            // layout space, which a zoomed or panned ancestor no longer draws them at.
            clip_children(*record->local_clip);
        }
        // Descendants are hit only where they are drawn: a widget that clips them further than
        // its layout does (rows below a table's sticky header) clips their hits the same way.
        if (commands_ != nullptr) {
            if (const std::optional<Rect> descendant_clip =
                    widget_descendant_clip(widgets_, node, *record, *layout_, *this, *commands_,
                                           nullptr, nullptr, motion_, opacity);
                descendant_clip.has_value()) {
                clip_children(*descendant_clip);
            }
        }
        std::vector<RetainedNode*> children;
        children.reserve(node.children().size());
        for (const auto& child : node.children())
            children.push_back(child.get());
        const auto z_order = [this](const RetainedNode* left, const RetainedNode* right) {
            const LayoutRecord* left_layout = layout_->find(left->identity());
            const LayoutRecord* right_layout = layout_->find(right->identity());
            const auto key = [](const LayoutRecord* record) {
                return record != nullptr ? std::pair{record->z_index, record->positioned}
                                         : std::pair{0, false};
            };
            return key(left_layout) < key(right_layout);
        };
        // Children are almost always already in z-order; a stable sort would leave them as they
        // are, so skip it and its temporary buffer.
        if (!std::ranges::is_sorted(children, z_order))
            std::ranges::stable_sort(children, z_order);
        // A clip shape cuts what is hit below it as well as the node itself.
        ClipChain child_clips = clips;
        if (std::vector<std::vector<Point>> own = clip_outlines(node, record->bounds);
            !own.empty()) {
            auto chain = clips != nullptr ? std::make_shared<std::vector<ClipOutline>>(*clips)
                                          : std::make_shared<std::vector<ClipOutline>>();
            chain->push_back(ClipOutline{node.identity(), std::move(own)});
            child_clips = std::move(chain);
        }
        for (auto child = children.rbegin(); child != children.rend(); ++child) {
            self(self, **child, visit_portals, transform, opacity, child_clip,
                 Anchored{here.anchor, here.relative, child_relative_clip}, child_clips);
        }
        const Rect bounds =
            hit_bounds_resolver_ ? hit_bounds_resolver_(node, *record) : record->hit_bounds;
        pointer_hit_index_.insert_or_assign(node.identity(), pointer_hit_entries_.size());
        pointer_hit_entries_.push_back(PointerHitEntry{
            &node,
            transform_presentation_bounds(bounds, transform),
            traversal_clip,
            transform,
            bounds,
            here.anchor,
            anchor,
            here.relative,
            anchor ? std::nullopt : here.relative_clip,
            hit_outlines(node, bounds),
            clips,
        });
    };
    // Detached portal roots occupy the final render plane. Visit them first in reverse paint
    // order so hit testing observes the same topmost-first order as rendering.
    for (auto portal = portals.rbegin(); portal != portals.rend(); ++portal) {
        visit(visit, **portal, true, MotionTransform{}, 1.0, std::nullopt, Anchored{}, nullptr);
    }
    visit(visit, *tree_->root(), false, MotionTransform{}, 1.0, std::nullopt, Anchored{},
          nullptr);
    pointer_geometry_tree_ = tree_;
    pointer_geometry_layout_ = layout_;
    pointer_geometry_motion_ = motion_;
    pointer_geometry_tree_generation_ = tree_->generation();
    pointer_geometry_layout_generation_ = layout_->generation;
    pointer_geometry_notification_generation_ = notifications;
    pointer_geometry_command_generation_ = command_generation;
    pointer_geometry_dirty_generations_ = dirty;
    pointer_geometry_ready_ = true;
    ++pointer_geometry_rebuild_count_;
}

void InputRouter::settle_pointer_geometry() const {
    if (!pointer_geometry_ready_ || tree_ == nullptr || tree_->root() == nullptr ||
        layout_ == nullptr) {
        return;
    }
    const DirtyGenerationSnapshot dirty = tree_->dirty_generations();
    if (pointer_geometry_dirty_generations_.animation == dirty.animation)
        return;
    // Only placing: geometry that must be built again is built when next asked for, as before.
    const std::uint64_t notifications = notifications_.generation();
    const std::uint64_t command_generation = commands_ != nullptr ? commands_->generation() : 0U;
    if (pointer_geometry_tree_ == tree_ && pointer_geometry_layout_ == layout_ &&
        pointer_geometry_motion_ == motion_ &&
        pointer_geometry_tree_generation_ == tree_->generation() &&
        pointer_geometry_layout_generation_ == layout_->generation &&
        pointer_geometry_notification_generation_ == notifications &&
        pointer_geometry_command_generation_ == command_generation && place_moved_anchors()) {
        pointer_geometry_dirty_generations_.animation = dirty.animation;
    }
}

bool InputRouter::place_moved_anchors() const {
    // Only the animation lane moved, and the frame that moved it still holds its marks, so every
    // node that changed is known.
    const DirtyGenerationSnapshot dirty = tree_->dirty_generations();
    DirtyGenerationSnapshot others = pointer_geometry_dirty_generations_;
    others.animation = dirty.animation;
    others.input = dirty.input;
    others.semantics = dirty.semantics;
    others.paint = dirty.paint;
    if (!(others == dirty) || pointer_anchors_.empty() || motion_ == nullptr)
        return false;
    const std::vector<RetainedNode*> changed = tree_->dirty_nodes();
    if (changed.empty())
        return false;
    const auto is_anchor = [this](const RetainedNode& node) {
        return local_presentation_tone(node, *motion_).has_value() ||
               motion_presentation_group(node, *motion_) || presentation_in_depth(node, *motion_) ||
               parent_content_view(node).has_value();
    };
    std::vector<bool> placed(pointer_anchors_.size(), false);
    bool any = false;
    for (const RetainedNode* node : changed) {
        const LayoutRecord* record = layout_->find(node->identity());
        if (record == nullptr)
            return false;
        const MotionTransform local = local_presentation_transform(*node, *motion_, record->bounds);
        const double opacity = local_presentation_opacity(*node, motion_);
        const auto anchor =
            std::ranges::find(pointer_anchors_, node->identity(), &PointerAnchor::identity);
        if (anchor == pointer_anchors_.end()) {
            // Anything else moves what is within it only by being presented, which it must not
            // be: a node presented with a transform or a fade is built again.
            if (is_anchor(*node) || !(local == MotionTransform{}) || opacity != 1.0)
                return false;
            continue;
        }
        // An anchor that turns, fades in or out, or stops being one changes more than where it is.
        if (!is_anchor(*node) || !local.axis_aligned() || !anchor->visible ||
            anchor->inherited_opacity * opacity <= 1.0e-3) {
            return false;
        }
        if (!(anchor->local == local)) {
            anchor->local = local;
            placed[static_cast<std::size_t>(anchor - pointer_anchors_.begin())] = true;
            any = true;
        }
    }
    if (!any)
        return true;
    // Anchors are in tree order: a parent is placed before the anchors within it.
    for (std::size_t index = 0U; index < pointer_anchors_.size(); ++index) {
        PointerAnchor& anchor = pointer_anchors_[index];
        const PointerAnchor* parent =
            anchor.parent >= 0 ? &pointer_anchors_[static_cast<std::size_t>(anchor.parent)]
                               : nullptr;
        if (parent != nullptr && placed[static_cast<std::size_t>(anchor.parent)])
            placed[index] = true;
        if (!placed[index])
            continue;
        const MotionTransform parent_world = parent != nullptr ? parent->world : MotionTransform{};
        const auto within = [&parent_world](std::optional<Rect> clip,
                                            const std::optional<Rect>& relative) {
            if (!relative.has_value())
                return clip;
            const Rect placed_clip = transform_presentation_bounds(*relative, parent_world);
            return clip.has_value() ? std::optional(clip->clip_intersection(placed_clip))
                                    : std::optional(placed_clip);
        };
        anchor.world = concatenate_presentation_transform(
            parent_world, concatenate_presentation_transform(anchor.base, anchor.local));
        if (!anchor.world.axis_aligned())
            return false;
        anchor.world_clip =
            within(parent != nullptr ? parent->world_inner_clip : std::nullopt, anchor.clip);
        anchor.world_inner_clip = within(anchor.world_clip, anchor.own_clip);
    }
    for (PointerHitEntry& entry : pointer_hit_entries_) {
        if (entry.anchor < 0 || !placed[static_cast<std::size_t>(entry.anchor)])
            continue;
        const PointerAnchor& anchor = pointer_anchors_[static_cast<std::size_t>(entry.anchor)];
        entry.transform = concatenate_presentation_transform(anchor.world, entry.relative);
        entry.bounds = transform_presentation_bounds(entry.local_bounds, entry.transform);
        std::optional<Rect> clip = entry.anchor_root ? anchor.world_clip : anchor.world_inner_clip;
        if (entry.relative_clip.has_value()) {
            const Rect placed_clip = transform_presentation_bounds(*entry.relative_clip, anchor.world);
            clip = clip.has_value() ? std::optional(clip->clip_intersection(placed_clip))
                                    : std::optional(placed_clip);
        }
        entry.traversal_clip = clip;
    }
    for (const PointerPlacement& placement : pointer_placements_) {
        if (placement.anchor < 0 || !placed[static_cast<std::size_t>(placement.anchor)])
            continue;
        presentation_transforms_.insert_or_assign(
            placement.identity,
            concatenate_presentation_transform(
                pointer_anchors_[static_cast<std::size_t>(placement.anchor)].world,
                placement.relative));
    }
    projected_subtargets_.clear();
    detached_subtargets_.clear();
    detached_subtargets_ready_ = false;
    return true;
}

const std::vector<WidgetSubtarget>&
InputRouter::projected_subtargets(const RetainedNode& node) const {
    static const std::vector<WidgetSubtarget> none;
    if (tree_ == nullptr || layout_ == nullptr)
        return none;
    prepare_pointer_geometry();
    const auto found = projected_subtargets_.find(node.identity());
    if (found != projected_subtargets_.end())
        return found->second;
    std::vector<WidgetSubtarget> targets;
    if (!inside_native_presentation(node) && node_participates(node))
        targets = project_subtargets(node);
    return projected_subtargets_.emplace(node.identity(), std::move(targets)).first->second;
}

std::vector<WidgetSubtarget> InputRouter::project_subtargets(const RetainedNode& node) const {
    const WidgetLifecycle* const lifecycle = widgets_.find(node.description().type);
    if (lifecycle == nullptr || lifecycle->inspection.subtargets == nullptr || layout_ == nullptr)
        return {};
    const LayoutRecord* const record = layout_->find(node.identity());
    if (record == nullptr)
        return {};
    ++subtarget_projection_count_;
    const std::string* query = edited_text(node.identity());
    return lifecycle->inspection.subtargets(WidgetSubtargetContext{
        node,
        *layout_,
        *record,
        commands_,
        &text_width_resolver_,
        &text_layout_resolver_,
        &notifications_,
        query != nullptr ? std::string_view(*query) : std::string_view{},
    });
}

void InputRouter::prepare_detached_subtargets() const {
    prepare_pointer_geometry();
    if (!pointer_geometry_ready_ || detached_subtargets_ready_)
        return;
    const std::vector<DetachedOverlayRoot> overlays =
        detached_overlay_roots(*tree_, *layout_, widgets_, widgets_.detached_subtarget_types(),
                               false, [this](const RetainedNode& node) {
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
                const auto entry = pointer_hit_index_.find(*presented.presentation_identity);
                if (entry == pointer_hit_index_.end())
                    continue;
                const PointerHitEntry& owner = pointer_hit_entries_[entry->second];
                presented.bounds = transform_presentation_bounds(presented.bounds, owner.transform);
                presented.presentation_clip = owner.traversal_clip;
            }
            detached_subtargets_.push_back(std::move(presented));
        }
    }
    detached_subtargets_ready_ = true;
}

bool InputRouter::inside_clips(const PointerHitEntry& entry, const Point position) const noexcept {
    if (entry.clips == nullptr)
        return true;
    return std::ranges::all_of(*entry.clips, [&](const ClipOutline& clip) {
        const auto transform = presentation_transforms_.find(clip.node);
        return inside_outlines(clip.outlines,
                               transform != presentation_transforms_.end()
                                   ? inverse_presentation_point(position, transform->second)
                                   : position);
    });
}

RetainedNode* InputRouter::hit_test(const Point position) const noexcept {
    prepare_pointer_geometry();
    for (const PointerHitEntry& entry : pointer_hit_entries_) {
        if ((!entry.traversal_clip.has_value() || contains(*entry.traversal_clip, position)) &&
            contains(entry.bounds, position) &&
            // A rotated node is hit inside its turned shape, not the box around it.
            (entry.transform.axis_aligned() ||
             contains(entry.local_bounds,
                      inverse_presentation_point(position, entry.transform))) &&
            // And a node with a hit shape only inside that.
            (entry.shape.empty() ||
             inside_outlines(entry.shape, inverse_presentation_point(position, entry.transform))) &&
            // And only inside the clip shapes of the nodes around it.
            inside_clips(entry, position)) {
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
    // Presentation, semantics and editor geometry ask every frame; a CommandPalette's projection
    // ranks all of its commands, so it is reused until something it depends on changes.
    if (!preparing_pointer_geometry_)
        return projected_subtargets(*node);
    return project_subtargets(*node);
}

bool InputRouter::subtarget_hovered(const std::uint64_t identity,
                                    const std::string_view id) const noexcept {
    return hovered_subtarget_.has_value() && hovered_subtarget_->first == identity &&
           hovered_subtarget_->second == id;
}

std::optional<std::string_view>
InputRouter::hovered_subtarget(const std::uint64_t identity) const noexcept {
    if (!hovered_subtarget_.has_value() || hovered_subtarget_->first != identity)
        return std::nullopt;
    return std::string_view(hovered_subtarget_->second);
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
            contains(target.bounds, position) && inside_subtarget_shape(target, position, {})) {
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
        if (projected.empty())
            continue;
        // Subtargets are laid out with their owner, so they are hit where its presentation
        // (a presence animation's scale or translation, say) currently draws it.
        const auto entry = pointer_hit_index_.find(current->identity());
        const PointerHitEntry* const owner =
            entry != pointer_hit_index_.end() ? &pointer_hit_entries_[entry->second] : nullptr;
        if (owner != nullptr && owner->traversal_clip.has_value() &&
            !contains(*owner->traversal_clip, position)) {
            continue;
        }
        for (auto candidate = projected.rbegin(); candidate != projected.rend(); ++candidate) {
            if (candidate->detached)
                continue;
            const Rect bounds = owner != nullptr
                                    ? transform_presentation_bounds(candidate->bounds,
                                                                    owner->transform)
                                    : candidate->bounds;
            if (!contains(bounds, position) ||
                !inside_subtarget_shape(*candidate, position,
                                        owner != nullptr ? owner->transform : MotionTransform{}))
                continue;
            // A modal backdrop cannot outrank a control inside that modal, regardless of zIndex.
            if (candidate->kind == WidgetSubtargetKind::scrim && ordinary_target != current)
                continue;
            if (!best.has_value() || candidate->z_index > best->z_index) {
                best = *candidate;
                best->bounds = bounds;
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
    hovered_subtarget_kind_ =
        target.has_value() ? std::optional(target->kind) : std::nullopt;
    if (tree_ != nullptr && hovered_subtarget_.has_value()) {
        static_cast<void>(tree_->mark(hovered_subtarget_->first, DirtyReason::paint));
    }
    if (frame_invalidator_)
        frame_invalidator_();
}

RetainedNode* InputRouter::subtarget_hover_target(const Point position, RetainedNode* const hit,
                                                  std::optional<WidgetSubtarget>& subtarget) {
    subtarget = hit_subtarget(position, hit);
    if (!subtarget.has_value())
        return hit;
    RetainedNode* owner = tree_->find_identity(subtarget->owner_identity);
    if (subtarget->kind == WidgetSubtargetKind::scrim && owner != nullptr && hit != nullptr &&
        hit != owner && descendant_of(*hit, *owner)) {
        subtarget.reset();
        return hit;
    }
    return owner != nullptr ? owner : hit;
}

void InputRouter::refresh_hover() {
    // A pressed, dragging or absent pointer hovers what its own events say.
    if (tree_ == nullptr || layout_ == nullptr || !hover_position_.has_value() ||
        !hover_present_ || !pressed_pointer_targets_.empty() || !drag_sessions_.empty() ||
        scrollbar_drag_.has_value()) {
        return;
    }
    // Hover is a function of the pointer and the hit geometry; when neither moved since the
    // last refresh, it is already right.
    const HoverRefreshKey key{*hover_position_, tree_->generation(), layout_->generation,
                              notifications_.generation(), tree_->dirty_generations(),
                              modal_identity()};
    if (last_hover_refresh_ == key)
        return;
    last_hover_refresh_ = key;
    std::optional<WidgetSubtarget> subtarget;
    RetainedNode* hover_target = subtarget_hover_target(
        *hover_position_, widget_native_input_owner(hit_test(*hover_position_)), subtarget);
    if (RetainedNode* modal = active_modal(); modal != nullptr) {
        const bool above_modal = subtarget.has_value() && subtarget->detached &&
                                 subtarget->owner_identity != modal->identity() &&
                                 subtarget->z_index > detached_overlay_z_index(widgets_, *modal);
        if (!above_modal && (hover_target == nullptr || !descendant_of(*hover_target, *modal)))
            hover_target = modal;
    }
    set_hovered_subtarget(subtarget);
    hover_route(hover_target);
}

std::uint64_t InputRouter::modal_identity() const noexcept {
    const RetainedNode* modal = active_modal();
    return modal != nullptr ? modal->identity() : 0U;
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
    if (event.type == PointerEventType::press ||
        (event.type == PointerEventType::move &&
         (!hover_position_.has_value() || *hover_position_ != event.position))) {
        restart_hover_disclosures(event.type == PointerEventType::press);
    }
    hover_position_ = event.position;
    hover_present_ = event.type != PointerEventType::cancel;
    // A press changes modality even when it lands on the already-focused control or is consumed.
    // Motion alone must not erase a keyboard user's location indicator.
    if (event.type == PointerEventType::press)
        set_focus_visibility(false);
    RetainedNode* const scrollbar_hit_target = widget_native_input_owner(hit_test(event.position));
    RetainedNode* hover_target =
        subtarget_hover_target(event.position, scrollbar_hit_target, routed_subtarget_);
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
            routed_subtarget_->z_index > detached_overlay_z_index(widgets_, *modal);
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
    // A press on a `strata.dragOut` node that moved past the slop leaves the window: the host
    // carries it from here, and cancels the press once its drag loop ends.
    if (drag_out_press_.has_value() && start_drag_out(event))
        return result;

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
        update_tooltip_disclosures();
    };

    if (event.type == PointerEventType::press) {
        hover_route(hover_target);
        dispatch_hover(result);
        track_pointer(event, result);
        dismiss_transient_popups(target, result);
        // Window gestures begin with the press, whatever takes it after: a title bar is often
        // no control at all, and a drag out may start on one.
        if (event.button == 0) {
            start_window_drag(event, hover_target);
            arm_drag_out(event, hover_target);
        }
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
        update_tooltip_disclosures();
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
    dispatch_hover(result);
    track_pointer(event, result);
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
                if (static_text_selection_.has_value() &&
                    static_text_selection_->source.has_value()) {
                    // A sourced selection follows the pointer anywhere, and scrolls when it
                    // is dragged past the edge of what shows the source.
                    extend_sourced_static_text_selection(hover_target, event.position);
                    track_static_text_drag(event.pointer_id, event.position);
                } else if (hover_target != nullptr && static_text_selection_.has_value()) {
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
        ((pressed->second.moved_beyond_slop && dispatch_target != nullptr &&
          (static_text_selectable(*dispatch_target) ||
           editors_.contains(dispatch_target->identity()))) ||
         pressed->second.long_press_emitted ||
         pressed->second.gesture != GestureClaimState::unclaimed);
    const bool subtarget_matches =
        pressed == pressed_pointer_targets_.end() ||
        pressed->second.subtarget_id ==
            (routed_subtarget_.has_value() ? routed_subtarget_->id : std::string{});
    finish_capture();
    hover_route(hover_target);
    dispatch_hover(result);
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
