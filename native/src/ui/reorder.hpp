#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/tree.hpp"

namespace strata::ui {

struct ReorderInsertion final {
    std::size_t index = 0U;
    double coordinate = 0.0;
    std::optional<std::string> before_key;
    std::optional<std::string> after_key;
};

[[nodiscard]] inline ReorderInsertion resolve_reorder_insertion(
    const RetainedNode& container,
    const LayoutResult& layout,
    const Point position,
    const bool horizontal
) {
    struct Child final {
        const RetainedNode* node;
        Rect bounds;
    };
    std::vector<Child> children;
    children.reserve(container.children().size());
    for (const auto& child : container.children()) {
        if (child->lifecycle() == RetainedLifecycle::exiting) continue;
        if (const LayoutRecord* record = layout.find(child->identity()); record != nullptr) {
            children.push_back(Child{child.get(), record->bounds});
        }
    }

    const double pointer = horizontal ? position.x : position.y;
    std::size_t index = children.size();
    for (std::size_t child_index = 0U; child_index < children.size(); ++child_index) {
        const Rect& bounds = children[child_index].bounds;
        const double midpoint = horizontal
            ? bounds.x + bounds.width * 0.5
            : bounds.y + bounds.height * 0.5;
        if (pointer < midpoint) {
            index = child_index;
            break;
        }
    }

    const LayoutRecord* container_layout = layout.find(container.identity());
    const Rect bounds = container_layout != nullptr ? container_layout->bounds : Rect{};
    const auto start = [horizontal](const Rect& value) {
        return horizontal ? value.x : value.y;
    };
    const auto end = [horizontal](const Rect& value) {
        return horizontal ? value.right() : value.bottom();
    };
    const double coordinate = children.empty()
        ? start(bounds)
        : index == 0U
            ? start(children.front().bounds)
            : index == children.size()
                ? end(children.back().bounds)
                : (end(children[index - 1U].bounds) + start(children[index].bounds)) * 0.5;

    ReorderInsertion result{index, coordinate};
    if (index < children.size() && children[index].node->description().key.has_value()) {
        result.before_key = *children[index].node->description().key;
    }
    if (index > 0U && children[index - 1U].node->description().key.has_value()) {
        result.after_key = *children[index - 1U].node->description().key;
    }
    return result;
}

} // namespace strata::ui
