#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "ui/input/detail.hpp"
#include "ui/layout.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"

// Selection over texts that are slices of one source text. A long document split across many
// texts (a code view's rows, realized only near the viewport) keeps its selection as offsets into
// the whole text instead of per realized text, so a selection reaches texts that are not
// realized: select-all and copy take the whole source, and a selection's ends survive their texts
// scrolling out of the realized window. Texts without a source keep the per-text model.

namespace strata::ui {
using namespace input_detail;
namespace {

/** Characters (not bytes) in text[start, end). */
[[nodiscard]] std::size_t character_count(const std::string_view text, const std::size_t start,
                                          const std::size_t end) noexcept {
    std::size_t count = 0U;
    for (std::size_t index = start; index < end && index < text.size(); ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xC0U) != 0x80U)
            ++count;
    }
    return count;
}

/** The offset `count` characters past `start`, no further than `end`. */
[[nodiscard]] std::size_t advance_characters(const std::string_view text, const std::size_t start,
                                             const std::size_t end, std::size_t count) noexcept {
    std::size_t at = start;
    while (count > 0U && at < end) {
        at = next_text_boundary(text, at);
        --count;
    }
    return std::min(at, end);
}

/**
 * Line-wise caret movement in plain text: home and end to the line's edges, up and down to the
 * same column of the line above or below. Used where the text holding the caret is not realized,
 * so there is no visual layout to move through.
 */
[[nodiscard]] std::size_t source_line_navigation(const std::string_view source,
                                                 const std::size_t focus,
                                                 const std::string_view key) noexcept {
    const std::size_t start = text_line_start(source, focus);
    const std::size_t end = text_line_end(source, focus);
    if (key == "home")
        return start;
    if (key == "end")
        return end;
    const std::size_t column = character_count(source, start, focus);
    if (key == "up") {
        if (start == 0U)
            return focus;
        const std::size_t previous_end = start - 1U;
        return advance_characters(source, text_line_start(source, previous_end), previous_end,
                                  column);
    }
    if (end >= source.size())
        return focus;
    const std::size_t next_start = end + 1U;
    return advance_characters(source, next_start, text_line_end(source, next_start), column);
}

/**
 * Scrolling speed for a pointer `position` against a viewport's [start, end]: nothing inside its
 * edge band, then faster the further in, and on beyond the edge, so a selection can be dragged
 * across a long document quickly.
 */
[[nodiscard]] double selection_edge_velocity(const double position, const double start,
                                             const double end,
                                             const InputProcessingConfig& config) noexcept {
    const double edge = std::max(1.0, std::min(config.drag_auto_scroll_edge, (end - start) * 0.25));
    constexpr double maximum_strength = 40.0;
    if (position < start + edge)
        return -std::min((start + edge - position) / edge, maximum_strength) *
               config.drag_auto_scroll_speed;
    if (position > end - edge)
        return std::min((position - (end - edge)) / edge, maximum_strength) *
               config.drag_auto_scroll_speed;
    return 0.0;
}

[[nodiscard]] Point clamp_into(const Point point, const Rect& bounds) noexcept {
    // Half a pixel inside, so the clamped point still hits what lies at the edge.
    const double inset_x = std::min(0.5, bounds.width * 0.5);
    const double inset_y = std::min(0.5, bounds.height * 0.5);
    return Point{
        std::clamp(point.x, bounds.x + inset_x, bounds.right() - inset_x),
        std::clamp(point.y, bounds.y + inset_y, bounds.bottom() - inset_y),
    };
}

[[nodiscard]] bool contains_point(const Rect& bounds, const Point point) noexcept {
    return point.x >= bounds.x && point.y >= bounds.y && point.x <= bounds.right() &&
           point.y <= bounds.bottom();
}

[[nodiscard]] double distance_to(const Rect& bounds, const Point point) noexcept {
    const double dx = point.x < bounds.x         ? bounds.x - point.x
                      : point.x > bounds.right() ? point.x - bounds.right()
                                                 : 0.0;
    const double dy = point.y < bounds.y          ? bounds.y - point.y
                      : point.y > bounds.bottom() ? point.y - bounds.bottom()
                                                  : 0.0;
    // Rows stack vertically: the row level with the point wins over one merely nearer sideways.
    return dy * 1.0e6 + dx;
}

} // namespace

std::optional<InputRouter::StaticTextSource>
InputRouter::static_text_source(const RetainedNode& node) const noexcept {
    if (!static_text_selectable(node) || !static_text_container(node).has_value())
        return std::nullopt;
    const runtime::Value* offset = scalar_property(node, "$sourceOffset");
    if (offset == nullptr || offset->number() == nullptr || !std::isfinite(*offset->number()) ||
        *offset->number() < 0.0) {
        return std::nullopt;
    }
    const std::optional<std::string_view> text = static_text_value(node);
    if (!text.has_value())
        return std::nullopt;
    const auto start = static_cast<std::size_t>(*offset->number());
    for (const RetainedNode* current = node.parent(); current != nullptr;
         current = current->parent()) {
        const runtime::Value* source = scalar_property(*current, "$selectionSource");
        if (source == nullptr || source->string() == nullptr)
            continue;
        const std::string_view whole = *source->string();
        // A text that is not where it claims to be in the source keeps the per-text model.
        if (start > whole.size() || text->size() > whole.size() - start)
            return std::nullopt;
        return StaticTextSource{current, whole, start};
    }
    return std::nullopt;
}

std::vector<RetainedNode*>
InputRouter::sourced_static_text_nodes(const RetainedNode& holder,
                                       const std::string_view container) const {
    std::vector<RetainedNode*> result;
    const auto visit = [this, container, &result](const auto& self,
                                                  const RetainedNode& node) -> void {
        for (const auto& child : node.children()) {
            const std::optional<std::string_view> candidate = static_text_container(*child);
            if (candidate.has_value() && *candidate == container &&
                scalar_property(*child, "$sourceOffset") != nullptr &&
                static_text_selectable(*child)) {
                result.push_back(child.get());
            }
            self(self, *child);
        }
    };
    visit(visit, holder);
    return result;
}

void InputRouter::apply_sourced_static_text_selection() {
    if (tree_ == nullptr || !static_text_selection_.has_value() ||
        !static_text_selection_->source.has_value() ||
        !static_text_selection_->container.has_value()) {
        return;
    }
    StaticTextSourceSelection& selection = *static_text_selection_->source;
    const RetainedNode* holder = tree_->find_identity(selection.holder_identity);
    const runtime::Value* source =
        holder != nullptr ? scalar_property(*holder, "$selectionSource") : nullptr;
    if (source == nullptr || source->string() == nullptr)
        return;
    const std::string_view whole = *source->string();
    // The source may have changed under the selection; its ends stay on character boundaries.
    selection.anchor = text_boundary(whole, selection.anchor);
    selection.focus = text_boundary(whole, selection.focus);
    const std::size_t low = std::min(selection.anchor, selection.focus);
    const std::size_t high = std::max(selection.anchor, selection.focus);
    bool changed = false;
    for (RetainedNode* node :
         sourced_static_text_nodes(*holder, *static_text_selection_->container)) {
        const std::optional<StaticTextSource> located = static_text_source(*node);
        const std::optional<std::string_view> text = static_text_value(*node);
        if (!located.has_value() || located->holder != holder || !text.has_value())
            continue;
        const std::size_t start = located->offset;
        const std::size_t end = start + text->size();
        // A collapsed selection is a caret in the one text holding it; a range lights every
        // text it overlaps with that text's part of it.
        const bool shares = low == high ? selection.focus >= start && selection.focus <= end
                                        : high > start && low < end;
        const std::uint64_t identity = node->identity();
        if (!shares) {
            if (static_text_ranges_.erase(identity) > 0U) {
                static_cast<void>(tree_->mark(identity, DirtyReason::input));
                changed = true;
            }
            continue;
        }
        const StaticTextRange next{
            text_boundary(*text, std::clamp(selection.anchor, start, end) - start),
            text_boundary(*text, std::clamp(selection.focus, start, end) - start),
        };
        const auto current = static_text_ranges_.find(identity);
        if (current == static_text_ranges_.end() || current->second.anchor != next.anchor ||
            current->second.focus != next.focus) {
            static_text_ranges_.insert_or_assign(identity, next);
            static_cast<void>(tree_->mark(identity, DirtyReason::input));
            changed = true;
        }
    }
    if (changed && frame_invalidator_)
        frame_invalidator_();
}

void InputRouter::extend_sourced_static_text_selection(RetainedNode* candidate,
                                                       const Point position) {
    if (tree_ == nullptr || layout_ == nullptr || !static_text_selection_.has_value() ||
        !static_text_selection_->source.has_value() ||
        !static_text_selection_->container.has_value()) {
        return;
    }
    const RetainedNode* holder =
        tree_->find_identity(static_text_selection_->source->holder_identity);
    if (holder == nullptr)
        return;
    // Beyond the source's viewport the pointer selects up to the edge of what is shown.
    Point point = position;
    if (const LayoutRecord* record = layout_->find(holder->identity());
        record != nullptr && record->clip.has_value() && record->clip->width > 0.0 &&
        record->clip->height > 0.0) {
        point = clamp_into(point, presented_bounds(*holder, *record->clip));
    }
    const auto belongs = [&](const RetainedNode& node) {
        const std::optional<StaticTextSource> located = static_text_source(node);
        return located.has_value() && located->holder == holder &&
               static_text_container(node) == static_text_selection_->container;
    };
    RetainedNode* target = nullptr;
    if (candidate != nullptr && belongs(*candidate)) {
        const LayoutRecord* record = layout_->find(candidate->identity());
        if (record != nullptr &&
            contains_point(presented_bounds(*candidate, record->bounds), point))
            target = candidate;
    }
    if (target == nullptr) {
        double nearest = std::numeric_limits<double>::infinity();
        for (RetainedNode* node :
             sourced_static_text_nodes(*holder, *static_text_selection_->container)) {
            const LayoutRecord* record = layout_->find(node->identity());
            if (record == nullptr)
                continue;
            const double distance = distance_to(presented_bounds(*node, record->bounds), point);
            if (distance < nearest) {
                nearest = distance;
                target = node;
            }
        }
    }
    if (target == nullptr)
        return;
    const std::optional<StaticTextSource> located = static_text_source(*target);
    const std::optional<std::string_view> text = static_text_value(*target);
    if (!located.has_value() || !text.has_value())
        return;
    const std::optional<std::size_t> offset = resolve_text_offset(*target, *text, point);
    if (!offset.has_value())
        return;
    static_text_selection_->source->focus = located->offset + text_boundary(*text, *offset);
    text_navigation_.erase(target->identity());
    apply_sourced_static_text_selection();
}

bool InputRouter::move_sourced_static_text_selection(RetainedNode& node,
                                                     const StaticTextSource& located,
                                                     const std::string_view key,
                                                     const KeyModifiers modifiers,
                                                     InputOperationResult& result) {
    const std::optional<std::string_view> container = static_text_container(node);
    if (!container.has_value())
        return false;
    if (!static_text_selection_.has_value() || !static_text_selection_->source.has_value() ||
        static_text_selection_->source->holder_identity != located.holder->identity()) {
        // Keyboard selection starting without a pointer one: from this text's own caret.
        const auto current = static_text_ranges_.find(node.identity());
        const std::size_t anchor =
            current != static_text_ranges_.end() ? current->second.anchor : 0U;
        const std::size_t focus = current != static_text_ranges_.end() ? current->second.focus : 0U;
        static_text_selection_ = StaticTextSelectionSession{
            node.identity(),
            focus,
            std::string(*container),
            StaticTextSourceSelection{located.holder->identity(), located.offset + anchor,
                                      located.offset + focus},
            std::nullopt,
        };
    }
    StaticTextSourceSelection& selection = *static_text_selection_->source;
    const std::string_view source = located.text;
    const std::size_t focus = text_boundary(source, selection.focus);
    const bool command = modifiers.control || modifiers.super_key;

    // The realized text holding the caret, for moving through its visual lines.
    struct Placed final {
        RetainedNode* node = nullptr;
        std::size_t start = 0U;
        std::string_view text;
    };
    const std::vector<RetainedNode*> realized =
        sourced_static_text_nodes(*located.holder, *container);
    const auto placed_at = [&](const std::size_t offset) -> std::optional<Placed> {
        for (RetainedNode* candidate : realized) {
            const std::optional<StaticTextSource> place = static_text_source(*candidate);
            const std::optional<std::string_view> text = static_text_value(*candidate);
            if (place.has_value() && text.has_value() && place->holder == located.holder &&
                offset >= place->offset && offset <= place->offset + text->size()) {
                return Placed{candidate, place->offset, *text};
            }
        }
        return std::nullopt;
    };

    std::size_t next = focus;
    if (key == "left") {
        next = previous_text_boundary(source, focus);
    } else if (key == "right") {
        next = next_text_boundary(source, focus);
    } else if ((key == "home" || key == "end") && command) {
        next = key == "home" ? 0U : source.size();
    } else if (const std::optional<Placed> here = placed_at(focus);
               !here.has_value() || !text_layout_resolver_) {
        next = source_line_navigation(source, focus, key);
    } else {
        const std::size_t local = focus - here->start;
        const TextLayout layout =
            text_layout_resolver_(*here->node, here->text, TextLayoutOptions{});
        const std::size_t utf16 = utf16_offset_for_utf8_byte(here->text, local);
        const auto retained = text_navigation_.find(here->node->identity());
        const bool remembered =
            retained != text_navigation_.end() && retained->second.caret == local;
        const std::size_t line =
            layout.lines.empty()
                ? 0U
                : text_layout_caret_line(layout, utf16,
                                         remembered ? std::optional(retained->second.line)
                                                    : std::nullopt);
        const bool crosses =
            (key == "up" && line == 0U) || (key == "down" && line + 1U >= layout.lines.size());
        if (!crosses || layout.lines.empty()) {
            const std::optional<std::size_t> visual =
                visual_text_navigation_offset(*here->node, here->text, local, key);
            next = visual.has_value() ? here->start + *visual
                                      : source_line_navigation(source, focus, key);
        } else {
            // Up from a text's first line or down from its last: into the next text, at the
            // same x, when it is realized; by the source's lines when it is not.
            const std::size_t end = here->start + here->text.size();
            const std::optional<std::size_t> beside =
                key == "up" ? (here->start > 0U ? std::optional(here->start - 1U) : std::nullopt)
                            : (end < source.size() ? std::optional(end + 1U) : std::nullopt);
            const std::optional<Placed> adjacent =
                beside.has_value() ? placed_at(*beside) : std::nullopt;
            if (!beside.has_value()) {
                next = focus;
            } else if (!adjacent.has_value() || adjacent->node == here->node) {
                next = source_line_navigation(source, focus, key);
            } else {
                double x_goal = remembered && retained->second.vertical_x_goal.has_value()
                                    ? *retained->second.vertical_x_goal
                                    : shaped_caret_x(layout.shaped, line, utf16);
                const LayoutRecord* from =
                    layout_ != nullptr ? layout_->find(here->node->identity()) : nullptr;
                const LayoutRecord* to =
                    layout_ != nullptr ? layout_->find(adjacent->node->identity()) : nullptr;
                if (from != nullptr && to != nullptr)
                    x_goal += from->content_bounds.x - to->content_bounds.x;
                const TextLayout target =
                    text_layout_resolver_(*adjacent->node, adjacent->text, TextLayoutOptions{});
                if (target.lines.empty()) {
                    next = source_line_navigation(source, focus, key);
                } else {
                    const std::size_t target_line = key == "up" ? target.lines.size() - 1U : 0U;
                    const std::size_t offset = utf8_byte_for_utf16_offset(
                        adjacent->text, text_layout_line_offset_at_x(target, target_line, x_goal));
                    text_navigation_.insert_or_assign(
                        adjacent->node->identity(),
                        TextNavigationState{offset, target_line, x_goal});
                    next = adjacent->start + offset;
                }
            }
        }
    }
    if (key == "left" || key == "right" || command) {
        for (RetainedNode* candidate : realized)
            text_navigation_.erase(candidate->identity());
    }
    if (!modifiers.shift) {
        selection.anchor = next;
        static_text_selection_->anchor_identity = node.identity();
    }
    selection.focus = next;
    apply_sourced_static_text_selection();

    // The caret stays in view: the line holding it, in the realized text holding it.
    if (const std::optional<Placed> caret = placed_at(next);
        caret.has_value() && text_layout_resolver_ && layout_ != nullptr) {
        const LayoutRecord* record = layout_->find(caret->node->identity());
        const TextLayout layout =
            text_layout_resolver_(*caret->node, caret->text, TextLayoutOptions{});
        if (record != nullptr && !layout.lines.empty()) {
            const std::size_t utf16 = utf16_offset_for_utf8_byte(caret->text, next - caret->start);
            const std::size_t line = text_layout_caret_line(layout, utf16);
            const TextLayoutLine& visual = layout.lines[line];
            const Rect revealed{
                record->content_bounds.x + shaped_caret_x(layout.shaped, line, utf16),
                record->content_bounds.y + visual.y,
                1.0,
                std::max(1.0, visual.height),
            };
            reveal_rect(*caret->node, revealed, 0.0, result);
        }
    }
    return true;
}

void InputRouter::track_static_text_drag(const std::int32_t pointer_id, const Point position) {
    if (!static_text_selection_.has_value() || !static_text_selection_->source.has_value())
        return;
    std::optional<StaticTextSelectionDrag>& drag = static_text_selection_->drag;
    if (!drag.has_value() || drag->pointer_id != pointer_id)
        drag = StaticTextSelectionDrag{pointer_id, position, std::nullopt, false};
    drag->position = position;
}

void InputRouter::auto_scroll_static_text_selection(InputOperationResult& result) {
    if (!static_text_selection_.has_value() || !static_text_selection_->drag.has_value())
        return;
    StaticTextSelectionDrag& drag = *static_text_selection_->drag;
    const RetainedNode* holder =
        tree_ != nullptr && static_text_selection_->source.has_value()
            ? tree_->find_identity(static_text_selection_->source->holder_identity)
            : nullptr;
    if (holder == nullptr || layout_ == nullptr ||
        !pressed_pointer_targets_.contains(drag.pointer_id)) {
        static_text_selection_->drag.reset();
        return;
    }
    // What scrolling brought under the pointer joins the selection, if layout has not yet
    // given it (see after_layout).
    if (drag.scrolled) {
        drag.scrolled = false;
        extend_sourced_static_text_selection(nullptr, drag.position);
    }
    const std::optional<std::int64_t> previous = drag.last_scroll_nanos;
    drag.last_scroll_nanos = frame_time_nanos_;
    if (!previous.has_value())
        return;
    const std::int64_t elapsed_nanos = std::clamp(frame_time_nanos_ - *previous, std::int64_t{0},
                                                  input_config_.drag_auto_scroll_max_frame_nanos);
    if (elapsed_nanos <= 0)
        return;
    const double elapsed_seconds = static_cast<double>(elapsed_nanos) / 1'000'000'000.0;
    // The innermost viewport that can still move toward the pointer scrolls.
    for (const RetainedNode* current = holder; current != nullptr; current = current->parent()) {
        const LayoutRecord* record = layout_->find(current->identity());
        const std::optional<Point> limits = scroll_limits(*current);
        if (record == nullptr || !record->viewport.has_value() || !limits.has_value())
            continue;
        const Rect viewport = presented_bounds(*current, *record->viewport);
        const LayoutStyle& style = LayoutEngine::node_style(*current);
        Point next = record->scroll_offset;
        if (style.scroll_horizontal) {
            next.x += selection_edge_velocity(drag.position.x, viewport.x, viewport.right(),
                                              input_config_) *
                      elapsed_seconds;
        }
        if (style.scroll_vertical) {
            next.y += selection_edge_velocity(drag.position.y, viewport.y, viewport.bottom(),
                                              input_config_) *
                      elapsed_seconds;
        }
        next.x = std::clamp(next.x, 0.0, limits->x);
        next.y = std::clamp(next.y, 0.0, limits->y);
        if (next.x == record->scroll_offset.x && next.y == record->scroll_offset.y)
            continue;
        if (!set_scroll_offset(*current, next, result))
            continue;
        drag.scrolled = true;
        if (frame_invalidator_)
            frame_invalidator_();
        return;
    }
}

} // namespace strata::ui
