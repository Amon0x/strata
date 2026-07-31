#include "ui/behavior/collection_marquee.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ui/behavior/input.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/widget/input_collection_common.hpp"

namespace strata::ui {
namespace {

using namespace collection_input;

constexpr std::string_view marquee_key = "strata.collection.marquee";

struct ItemGeometry final {
    std::string key;
    Rect bounds;
};

[[nodiscard]] Rect between(const Point first, const Point second) noexcept {
    const double x = std::min(first.x, second.x);
    const double y = std::min(first.y, second.y);
    return Rect{x, y, std::abs(first.x - second.x), std::abs(first.y - second.y)};
}

[[nodiscard]] bool intersects(const Rect& left, const Rect& right) noexcept {
    return left.x <= right.right() && left.right() >= right.x &&
        left.y <= right.bottom() && left.bottom() >= right.y;
}

[[nodiscard]] double distance_squared(const Rect& bounds, const Point point) noexcept {
    const double x = std::clamp(point.x, bounds.x, bounds.right());
    const double y = std::clamp(point.y, bounds.y, bounds.bottom());
    const double dx = point.x - x;
    const double dy = point.y - y;
    return dx * dx + dy * dy;
}

[[nodiscard]] std::vector<ItemGeometry> geometry(
    WidgetInputScope& scope,
    const Rect selection
) {
    std::vector<ItemGeometry> result;
    const LayoutRecord* root = scope.layout();
    if (root != nullptr && root->virtual_axis == LayoutAxis::vertical &&
        root->virtual_item_extents.has_value() &&
        root->virtual_item_members != nullptr &&
        root->virtual_item_members->size() == root->virtual_item_extents->size()) {
        const collection::VirtualItemExtents& extents = *root->virtual_item_extents;
        const double gap = std::max(0.0, scope.number("gap", 6.0));
        const double cell_width = std::max(0.0, scope.number("cellWidth", 112.0));
        const double cell_height = std::max(0.0, scope.number("cellHeight", 88.0));
        const double local_start = selection.y - root->content_bounds.y;
        const double local_end = selection.bottom() - root->content_bounds.y;
        const std::size_t first = std::min(
            extents.size(),
            extents.first_index_ending_after(std::max(0.0, local_start))
        );
        const std::size_t end = std::min(
            extents.size(),
            extents.end_index_starting_before(
                std::clamp(local_end, 0.0, extents.total())
            )
        );
        for (std::size_t band = first; band < end; ++band) {
            const std::vector<std::string>& members = (*root->virtual_item_members)[band];
            const double y = root->content_bounds.y + extents.start(band);
            const double height = std::min(cell_height, extents.extent(band));
            for (std::size_t column = 0U; column < members.size(); ++column) {
                const Rect bounds{
                    root->content_bounds.x +
                        static_cast<double>(column) * (cell_width + gap),
                    y,
                    cell_width,
                    height,
                };
                if (intersects(selection, bounds)) {
                    result.push_back(ItemGeometry{members[column], bounds});
                }
            }
        }
        return result;
    }
    const Point offset = root != nullptr ? root->scroll_offset : Point{};
    for (const auto& band : scope.node().children()) {
        // Non-virtual/custom collection implementations retain their full selectable geometry.
        for (const auto& item : band->children()) {
            if (item->description().key.has_value()) {
                if (const LayoutRecord* layout = scope.layout(*item); layout != nullptr) {
                    Rect bounds = layout->bounds;
                    bounds.x += offset.x;
                    bounds.y += offset.y;
                    result.push_back(ItemGeometry{*item->description().key, bounds});
                }
            }
        }
    }
    return result;
}

[[nodiscard]] bool pointer_on_item(WidgetInputScope& scope) noexcept {
    for (RetainedNode* current = scope.pointer_target();
         current != nullptr && current != &scope.node();
         current = current->parent()) {
        RetainedNode* band = current->parent();
        if (band != nullptr && band->parent() == &scope.node()) return true;
    }
    return false;
}

[[nodiscard]] std::optional<std::string> nearest(
    const collection::KeySet& hits,
    const Point point,
    const std::vector<ItemGeometry>& items
) {
    std::optional<std::string> result;
    double best = std::numeric_limits<double>::infinity();
    for (const ItemGeometry& item : items) {
        if (!hits.contains(item.key)) continue;
        const double distance = distance_squared(item.bounds, point);
        if (distance < best) {
            best = distance;
            result = item.key;
        }
    }
    return result;
}

[[nodiscard]] runtime::Value point_value(const Point point) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
        {"x", runtime::Value(point.x)},
        {"y", runtime::Value(point.y)},
    });
}

[[nodiscard]] std::optional<Point> point(const runtime::Value* value) noexcept {
    const double* x = value != nullptr && value->field("x") != nullptr
        ? value->field("x")->number()
        : nullptr;
    const double* y = value != nullptr && value->field("y") != nullptr
        ? value->field("y")->number()
        : nullptr;
    return x != nullptr && y != nullptr ? std::optional<Point>(Point{*x, *y}) : std::nullopt;
}

[[nodiscard]] std::optional<std::string> optional_key(const runtime::Value* value) {
    const std::string* key = text(value);
    return key != nullptr && !key->empty()
        ? std::optional<std::string>(*key)
        : std::nullopt;
}

[[nodiscard]] runtime::Value session_value(
    const Point start_viewport,
    const Point start_content,
    const Point current_viewport,
    const collection::KeySet& base,
    const std::optional<std::string>& anchor,
    const std::optional<std::string>& active,
    const KeyModifiers modifiers,
    const std::int64_t frame_time_nanos
) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
        {"baseActive", active.has_value() ? key_value(*active) : runtime::Value{}},
        {"baseAnchor", anchor.has_value() ? key_value(*anchor) : runtime::Value{}},
        {"baseSelection", key_list(base)},
        {"command", runtime::Value(modifiers.control || modifiers.super_key)},
        {"currentViewport", point_value(current_viewport)},
        {"lastFrame", runtime::Value(static_cast<double>(frame_time_nanos))},
        {"lastScroll", point_value(Point{
            start_content.x - start_viewport.x,
            start_content.y - start_viewport.y,
        })},
        {"lastSelection", key_list(base)},
        {"shift", runtime::Value(modifiers.shift)},
        {"startContent", point_value(start_content)},
        {"startViewport", point_value(start_viewport)},
    });
}

void update_session_field(
    WidgetInputScope& scope,
    const runtime::Value& session,
    const std::string_view name,
    runtime::Value value
) {
    std::vector<std::pair<std::string, runtime::Value>> fields = session.object()->fields;
    const auto found = std::ranges::find_if(fields, [name](const auto& field) {
        return field.first == name;
    });
    if (found != fields.end()) found->second = std::move(value);
    else fields.emplace_back(std::string(name), std::move(value));
    scope.set_presentation(std::string(marquee_key), runtime::Value(std::move(fields)));
}

[[nodiscard]] collection::SelectionTransition marquee_transition(
    WidgetInputScope& scope,
    const runtime::Value& session,
    const Point current_viewport
) {
    const LayoutRecord* root = scope.layout();
    const Point offset = root != nullptr ? root->scroll_offset : Point{};
    const Point current_content{current_viewport.x + offset.x, current_viewport.y + offset.y};
    const Point start = point(session.field("startContent")).value_or(current_content);
    const Rect marquee = between(start, current_content);
    const std::vector<ItemGeometry> items = geometry(scope, marquee);
    collection::KeySet hits;
    for (const ItemGeometry& item : items) {
        if (intersects(marquee, item.bounds)) static_cast<void>(hits.insert(item.key));
    }
    const collection::SelectionMode mode = selection_mode(scope);
    const bool additive =
        (session.field("command") != nullptr && session.field("command")->boolean() != nullptr &&
         *session.field("command")->boolean()) ||
        (session.field("shift") != nullptr && session.field("shift")->boolean() != nullptr &&
         *session.field("shift")->boolean());
    const std::optional<std::string> active = nearest(hits, current_content, items);
    const std::optional<std::string> anchor = nearest(hits, start, items);
    if (mode == collection::SelectionMode::none) {
        return collection::SelectionTransition{{}, std::nullopt, active};
    }
    if (mode == collection::SelectionMode::single) {
        collection::KeySet one;
        if (active.has_value()) static_cast<void>(one.insert(*active));
        return collection::SelectionTransition{std::move(one), active, active};
    }
    collection::KeySet next = additive ? keys(session.field("baseSelection")) : collection::KeySet{};
    for (const std::string& key : hits.values()) static_cast<void>(next.insert(key));
    return collection::SelectionTransition{
        std::move(next),
        anchor.has_value() ? anchor : additive ? optional_key(session.field("baseAnchor")) : std::nullopt,
        active.has_value() ? active : additive ? optional_key(session.field("baseActive")) : std::nullopt,
    };
}

void update_session(
    WidgetInputScope& scope,
    const runtime::Value& session,
    const Point current,
    const collection::SelectionTransition& transition
) {
    std::vector<std::pair<std::string, runtime::Value>> fields = session.object()->fields;
    const LayoutRecord* root = scope.layout();
    const Point scroll = root != nullptr ? root->scroll_offset : Point{};
    for (auto& [name, value] : fields) {
        if (name == "currentViewport") value = point_value(current);
        else if (name == "lastScroll") value = point_value(scroll);
        else if (name == "lastSelection") value = key_list(transition.selected);
    }
    scope.set_presentation(std::string(marquee_key), runtime::Value(std::move(fields)));
}

[[nodiscard]] collection::SelectionTransition preview_marquee(
    WidgetInputScope& scope,
    const runtime::Value& session,
    const Point current
) {
    collection::SelectionTransition transition = marquee_transition(
        scope, session, current
    );
    update_session(scope, session, current, transition);
    return transition;
}

bool marquee_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* event = scope.pointer();
    if (event == nullptr) return false;
    const runtime::Value* active_session = scope.retained(marquee_key);
    const bool has_session = active_session != nullptr && active_session->object() != nullptr;
    if (event->type == PointerEventType::press) {
        if (event->button != 0 || pointer_on_item(scope)) return false;
        const LayoutRecord* root = scope.layout();
        const Point offset = root != nullptr ? root->scroll_offset : Point{};
        const Point content{event->position.x + offset.x, event->position.y + offset.y};
        scope.set_presentation(
            std::string(marquee_key),
            session_value(
                event->position,
                content,
                event->position,
                selected(scope),
                retained_key(scope, "strata.collection.anchor"),
                retained_key(scope, "strata.collection.active"),
                event->modifiers,
                scope.frame_time_nanos()
            )
        );
        return true;
    }
    if (!has_session) return false;
    if (event->type == PointerEventType::move) {
        static_cast<void>(preview_marquee(scope, *active_session, event->position));
        return true;
    }
    if (event->type == PointerEventType::release && event->button == 0) {
        const collection::SelectionTransition transition = marquee_transition(
            scope, *active_session, event->position
        );
        commit_selection(scope, selected(scope), transition);
        scope.set_presentation(std::string(marquee_key), runtime::Value{});
        return true;
    }
    if (event->type == PointerEventType::cancel) return cancel_collection_marquee(scope);
    return true;
}

[[nodiscard]] double edge_velocity(
    const double position,
    const double start,
    const double end,
    const double inset,
    const double speed
) noexcept {
    if (position < start + inset) {
        return -std::clamp((start + inset - position) / inset, 0.0, 1.0) * speed;
    }
    if (position > end - inset) {
        return std::clamp((position - (end - inset)) / inset, 0.0, 1.0) * speed;
    }
    return 0.0;
}

bool advance(WidgetInputScope& scope) {
    const runtime::Value* active_session = scope.retained(marquee_key);
    const LayoutRecord* layout = scope.layout();
    if (active_session == nullptr || active_session->object() == nullptr || layout == nullptr ||
        !layout->viewport.has_value()) {
        return false;
    }
    const std::optional<Point> current = point(active_session->field("currentViewport"));
    if (!current.has_value()) return false;
    const double inset = std::max(1.0, scope.number("marqueeEdgeInset", 28.0));
    const double speed = std::max(1.0, scope.number("marqueeMaximumScrollSpeed", 480.0));
    const Rect& viewport = *layout->viewport;
    const double velocity_x = edge_velocity(
        current->x, viewport.x, viewport.right(), inset, speed
    );
    const double velocity_y = edge_velocity(
        current->y, viewport.y, viewport.bottom(), inset, speed
    );
    if (velocity_x == 0.0 && velocity_y == 0.0) return false;
    const runtime::Value* prior_value = active_session->field("lastFrame");
    const std::int64_t prior = prior_value != nullptr && prior_value->number() != nullptr
        ? static_cast<std::int64_t>(*prior_value->number()) : 0;
    const std::int64_t now = scope.frame_time_nanos();
    update_session_field(scope, *active_session, "lastFrame", runtime::Value(static_cast<double>(now)));
    if (prior <= 0 || now <= prior) return true;
    const double seconds = static_cast<double>(std::min<std::int64_t>(now - prior, 50'000'000)) /
        1'000'000'000.0;
    return scope.scroll_by(
        velocity_x * seconds,
        velocity_y * seconds
    );
}

bool after_layout(WidgetInputScope& scope) {
    const runtime::Value* active_session = scope.retained(marquee_key);
    if (active_session == nullptr || active_session->object() == nullptr) return false;
    const LayoutRecord* layout = scope.layout();
    if (layout == nullptr) return false;
    const std::optional<Point> current = point(active_session->field("currentViewport"));
    if (!current.has_value()) return false;
    const std::optional<Point> previous_scroll = point(active_session->field("lastScroll"));
    if (previous_scroll.has_value() && previous_scroll->x == layout->scroll_offset.x &&
        previous_scroll->y == layout->scroll_offset.y) {
        return false;
    }
    static_cast<void>(preview_marquee(scope, *active_session, *current));
    return true;
}

bool behavior_pointer(BehaviorInputScope& behavior) {
    WidgetInputScope scope = behavior.widget_scope();
    const bool handled = marquee_pointer(scope);
    if (handled && behavior.event().type == PointerEventType::move) {
        static_cast<void>(behavior.claim_gesture());
    } else if (handled && behavior.event().type == PointerEventType::cancel) {
        static_cast<void>(behavior.cancel_gesture());
    }
    // Let the router establish normal pointer ownership after the behavior starts its session.
    return behavior.event().type != PointerEventType::press && handled;
}

bool behavior_advance(BehaviorInputScope& behavior) {
    WidgetInputScope scope = behavior.widget_scope();
    return advance(scope);
}

bool behavior_after_layout(BehaviorInputScope& behavior) {
    WidgetInputScope scope = behavior.widget_scope();
    return after_layout(scope);
}

} // namespace

bool cancel_collection_marquee(WidgetInputScope& scope) {
    const runtime::Value* active_session = scope.retained(marquee_key);
    if (active_session == nullptr || active_session->object() == nullptr) return false;
    scope.set_presentation(std::string(marquee_key), runtime::Value{});
    return true;
}

void register_collection_behavior_inputs(BehaviorRegistry& registry) {
    BehaviorInputPhase phase;
    phase.pointer = &behavior_pointer;
    phase.advance = &behavior_advance;
    phase.after_layout = &behavior_after_layout;
    phase.accepts_pointer = true;
    registry.register_input_phase("strata.collection-marquee", std::move(phase));
}

} // namespace strata::ui
