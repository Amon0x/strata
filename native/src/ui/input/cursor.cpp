#include "ui/input.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include "runtime/value.hpp"
#include "ui/input/detail.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

constexpr std::array<std::pair<PointerCursor, std::string_view>, 16U> cursor_names{{
    {PointerCursor::standard, "DEFAULT"},
    {PointerCursor::pointer, "POINTER"},
    {PointerCursor::text, "TEXT"},
    {PointerCursor::grab, "GRAB"},
    {PointerCursor::grabbing, "GRABBING"},
    {PointerCursor::move, "MOVE"},
    {PointerCursor::not_allowed, "NOT_ALLOWED"},
    {PointerCursor::resize_ew, "RESIZE_EW"},
    {PointerCursor::resize_ns, "RESIZE_NS"},
    {PointerCursor::resize_nesw, "RESIZE_NESW"},
    {PointerCursor::resize_nwse, "RESIZE_NWSE"},
    {PointerCursor::crosshair, "CROSSHAIR"},
    {PointerCursor::wait, "WAIT"},
    {PointerCursor::progress, "PROGRESS"},
    {PointerCursor::help, "HELP"},
    {PointerCursor::none, "NONE"},
}};

/** The `cursor` a node's style or layout authors, which wins over every inferred shape. */
[[nodiscard]] std::optional<PointerCursor> authored_cursor(const RetainedNode& node) {
    const runtime::Value* layout = scalar_property(node, "$layout");
    const runtime::Value* value = layout != nullptr ? layout->field("cursor") : nullptr;
    if (value == nullptr || value->string() == nullptr)
        return std::nullopt;
    return pointer_cursor_from_name(*value->string());
}

} // namespace

std::string_view pointer_cursor_name(const PointerCursor cursor) noexcept {
    for (const auto& [value, name] : cursor_names) {
        if (value == cursor)
            return name;
    }
    return "DEFAULT";
}

std::optional<PointerCursor> pointer_cursor_from_name(const std::string_view name) noexcept {
    for (const auto& [value, candidate] : cursor_names) {
        if (candidate == name)
            return value;
    }
    return std::nullopt;
}

PointerCursor InputRouter::cursor() const {
    for (const auto& [pointer, session] : drag_sessions_) {
        static_cast<void>(pointer);
        if (session.active)
            return PointerCursor::grabbing;
    }
    if (tree_ == nullptr || !hover_target_.has_value())
        return PointerCursor::standard;
    const RetainedNode* const target = tree_->find_identity(*hover_target_);
    for (const RetainedNode* node = target; node != nullptr; node = node->parent()) {
        if (const std::optional<PointerCursor> authored = authored_cursor(*node);
            authored.has_value()) {
            return *authored;
        }
    }
    for (const RetainedNode* node = target; node != nullptr; node = node->parent()) {
        if (hovered_subtarget_.has_value() && hovered_subtarget_->first == node->identity() &&
            hovered_subtarget_kind_ == WidgetSubtargetKind::link) {
            return PointerCursor::pointer;
        }
        if (editors_.contains(node->identity()) || static_text_selectable(*node))
            return PointerCursor::text;
        if (behavior(*node, "strata.drag-source") != nullptr)
            return PointerCursor::grab;
        if (behavior(*node, "strata.movable") != nullptr)
            return PointerCursor::move;
    }
    return PointerCursor::standard;
}

} // namespace strata::ui
