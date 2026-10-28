#include "ui/widget/subtarget.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <utility>

#include "ui/text_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {

std::vector<RichTextLink> rich_text_links(const RetainedNode& node) {
    std::vector<RichTextLink> result;
    const auto found = node.description().properties.find("spans");
    if (found == node.description().properties.end() || found->second.list() == nullptr) {
        return result;
    }
    std::size_t byte_offset = 0U;
    std::size_t utf16_offset = 0U;
    const runtime::ExpressionListValue& spans = **found->second.list();
    for (std::size_t index = 0U; index < spans.values.size(); ++index) {
        const runtime::ExpressionValue& span = spans.values[index];
        const runtime::Value* scalar = span.value();
        const runtime::ExpressionValue* expressed_text = span.object() != nullptr
            ? (**span.object()).field("text")
            : nullptr;
        const runtime::Value* span_text = expressed_text != nullptr
            ? expressed_text->value()
            : scalar != nullptr ? scalar->field("text") : nullptr;
        const std::string_view value = span_text != nullptr && span_text->string() != nullptr
            ? std::string_view(*span_text->string())
            : std::string_view{};
        const std::size_t byte_end = byte_offset + value.size();
        const std::size_t utf16_end = utf16_offset + utf16_offset_for_utf8_byte(
            value, value.size()
        );
        const runtime::ExpressionValue* action = span.object() != nullptr
            ? (**span.object()).field("action") : nullptr;
        if (action != nullptr && action->action() != nullptr && *action->action() != nullptr &&
            (*action->action())->action != nullptr) {
            result.push_back(RichTextLink{
                index,
                byte_offset,
                byte_end,
                utf16_offset,
                utf16_end,
                *action->action(),
            });
        }
        byte_offset = byte_end;
        utf16_offset = utf16_end;
    }
    return result;
}

int detached_overlay_z_index(const WidgetRegistry& widgets, const RetainedNode& node) noexcept {
    const WidgetLifecycle* lifecycle = widgets.find(node.description().type);
    return lifecycle != nullptr ? lifecycle->present.overlay_z : detached_overlay_menu_z;
}

std::vector<DetachedOverlayRoot> detached_overlay_roots(
    const RetainedTree& tree,
    const LayoutResult& layout,
    const WidgetRegistry& widgets,
    const std::span<const std::string_view> candidate_types,
    const bool behavior_candidates,
    const DetachedOverlayPredicate& participates
) {
    std::vector<DetachedOverlayRoot> result;
    if (tree.root() == nullptr || !participates) return result;
    std::vector<RetainedNode*> candidates = tree.nodes_of_types(candidate_types);
    if (behavior_candidates && !tree.behavior_nodes().empty()) {
        const std::size_t typed = candidates.size();
        candidates.insert(
            candidates.end(), tree.behavior_nodes().begin(), tree.behavior_nodes().end()
        );
        const auto by_preorder = [](const RetainedNode* const left,
                                    const RetainedNode* const right) {
            return left->preorder() < right->preorder();
        };
        std::inplace_merge(candidates.begin(),
                           candidates.begin() + static_cast<std::ptrdiff_t>(typed),
                           candidates.end(), by_preorder);
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    }
    for (const RetainedNode* const node : candidates) {
        // Whether it has anything to show is cheap; whether it and every ancestor are laid out
        // is a walk.
        if (!participates(*node)) continue;
        bool laid_out = true;
        for (const RetainedNode* current = node; current != nullptr && laid_out;
             current = current->parent()) {
            laid_out = layout.find(current->identity()) != nullptr;
        }
        if (!laid_out) continue;
        result.push_back(DetachedOverlayRoot{
            node,
            detached_overlay_z_index(widgets, *node),
            node->preorder(),
        });
    }
    std::ranges::stable_sort(result, [](const DetachedOverlayRoot& left,
                                        const DetachedOverlayRoot& right) {
        return left.z_index != right.z_index
            ? left.z_index < right.z_index
            : left.author_order < right.author_order;
    });
    return result;
}

} // namespace strata::ui
