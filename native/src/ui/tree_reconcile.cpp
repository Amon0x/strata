#include "ui/tree.hpp"
#include "ui/text.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace strata::ui {
namespace {

struct SiblingKey final {
    std::string type;
    std::string key;
    [[nodiscard]] friend bool operator<(const SiblingKey& left, const SiblingKey& right) noexcept {
        if (left.type != right.type) return left.type < right.type;
        return left.key < right.key;
    }
};

[[nodiscard]] bool behaviors_equal(
    const std::vector<DescriptionBehavior>& left,
    const std::vector<DescriptionBehavior>& right
) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].id != right[index].id || left[index].enabled != right[index].enabled ||
            left[index].options != right[index].options ||
            !expression_value_equal(
                left[index].action != nullptr
                    ? runtime::ExpressionValue(left[index].action)
                    : runtime::ExpressionValue(runtime::Value{}),
                right[index].action != nullptr
                    ? runtime::ExpressionValue(right[index].action)
                    : runtime::ExpressionValue(runtime::Value{})
            )) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool layout_projection_equal(
    const runtime::ExpressionValue* current,
    const runtime::ExpressionValue* next
) noexcept {
    const runtime::Value* current_value = current != nullptr ? current->value() : nullptr;
    const runtime::Value* next_value = next != nullptr ? next->value() : nullptr;
    static constexpr std::array<std::string_view, 39U> fields{
        "kind", "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
        "aspectRatio", "intrinsicSize", "padding", "margin", "gap", "alignItems",
        "justifyContent", "alignContent", "alignSelf", "justifySelf", "wrap", "clip",
        "gridColumn", "gridRow", "columnSpan", "rowSpan", "zIndex", "scrollHorizontal",
        "scrollVertical", "viewportInsets", "viewportInsetsFromInsideBorder",
        "contentPadding", "scrollbarGutter",
        "virtualItemExtent", "virtualItemCount", "virtualAxis", "virtualOverscan",
        "virtualItemKeys", "virtualItemMembers", "virtualItemExtents",
        "virtualMeasureItemExtents", "$themeAnimationSet",
    };
    for (const std::string_view field : fields) {
        const runtime::Value* left = current_value != nullptr ? current_value->field(field) : nullptr;
        const runtime::Value* right = next_value != nullptr ? next_value->field(field) : nullptr;
        if ((left == nullptr) != (right == nullptr) ||
            (left != nullptr && *left != *right)) {
            return false;
        }
    }
    for (const std::string_view field : {
             "portalTarget",
             "detachFromParentClip",
             "anchorTarget",
             "anchorPoint",
             "anchorSide",
             "anchorAlign",
             "anchorGap",
             "anchorFlip",
             "anchorShift",
             "matchAnchorWidth",
         }) {
        const runtime::Value* left = current_value != nullptr ? current_value->field(field) : nullptr;
        const runtime::Value* right = next_value != nullptr ? next_value->field(field) : nullptr;
        if ((left == nullptr) != (right == nullptr) ||
            (left != nullptr && *left != *right)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] DirtyReason property_reason(
    const std::string_view name,
    const runtime::ExpressionValue* current,
    const runtime::ExpressionValue* next
) noexcept {
    if (text_layout_field(name)) {
        return DirtyReason::text;
    }
    if (name == "$layout") {
        if (!layout_projection_equal(current, next)) return DirtyReason::layout;
        return text_layout_projection_equal(
            current != nullptr ? current->value() : nullptr,
            next != nullptr ? next->value() : nullptr
        ) ? DirtyReason::properties : DirtyReason::text;
    }
    if (name == "textStyle") return DirtyReason::text;
    if (name == "style") {
        return text_layout_projection_equal(
            current != nullptr ? current->value() : nullptr,
            next != nullptr ? next->value() : nullptr
        ) ? DirtyReason::style : DirtyReason::text;
    }
    if (name == "layout" || name == "width" || name == "height" || name == "padding" ||
        name == "margin" || name == "gap" || name == "columns" || name == "rows" ||
        name == "scrollOffset" || name == "scrollPin" || name == "movement") {
        return DirtyReason::layout;
    }
    if (name == "variant") return DirtyReason::style;
    if (name == "semantics") return DirtyReason::semantics;
    return DirtyReason::properties;
}

[[nodiscard]] bool affects_layout(const DirtyReason reason) noexcept {
    return reason == DirtyReason::structure || reason == DirtyReason::layout ||
           reason == DirtyReason::text || reason == DirtyReason::style ||
           reason == DirtyReason::scale || reason == DirtyReason::resource;
}

[[nodiscard]] std::string child_path(
    const std::string_view parent,
    const std::size_t index
) {
    return parent == "/"
               ? "/" + std::to_string(index)
               : std::string(parent) + "/" + std::to_string(index);
}

} // namespace

ReconcileStats RetainedTree::reconcile(
    std::shared_ptr<const DescriptionNode> root,
    const ExitRetention& exit_retention
) {
    if (root == nullptr) throw std::invalid_argument("retained reconciliation requires a root description");
    ReconcileStats stats;
    bool layout_invalidated = false;
    root_ = reconcile_node(
        std::move(root_), std::move(root), nullptr, 0U, "/", stats,
        layout_invalidated, exit_retention
    );
    if (stats.changed()) {
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("retained tree generation exhausted");
        }
        ++generation_;
        invalidate_description_snapshot();
    }
    if (layout_invalidated) invalidate_layout();
    stats.generation = generation_;
    if (stats.changed()) rebuild_indexes();
    return stats;
}

std::unique_ptr<RetainedNode> RetainedTree::reconcile_node(
    std::unique_ptr<RetainedNode> existing,
    std::shared_ptr<const DescriptionNode> description,
    RetainedNode* const parent,
    const std::size_t source_index,
    std::string structural_path,
    ReconcileStats& stats,
    bool& layout_invalidated,
    const ExitRetention& exit_retention
) {
    const bool compatible = existing != nullptr &&
                            existing->description_->type == description->type &&
                            existing->description_->key == description->key;
    if (compatible && existing->description_ == description && existing->parent_ == parent &&
        existing->source_index_ == source_index &&
        existing->structural_path_ == structural_path &&
        existing->subtree_all_attached_) {
        stats.reused += existing->subtree_node_count_;
        stats.materialized += existing->subtree_materialized_child_count_;
        return existing;
    }
    bool node_updated = false;
    bool node_layout_updated = false;
    if (!compatible) {
        detach(std::move(existing), &stats);
        existing = std::unique_ptr<RetainedNode>(new RetainedNode(
            next_identity(), description, parent, source_index, std::move(structural_path)
        ));
        hydrate_persistence(*existing);
        ++stats.created;
        bump_dirty_generation(DirtyReason::structure);
        bump_dirty_generation(DirtyReason::properties);
        bump_dirty_generation(DirtyReason::layout);
        layout_invalidated = true;
    } else {
        ++stats.reused;
        if (existing->lifecycle_ == RetainedLifecycle::exiting) {
            mark_subtree_lifecycle(*existing, RetainedLifecycle::attached);
            existing->mark_dirty(DirtyReason::animation);
            bump_dirty_generation(DirtyReason::animation);
            node_updated = true;
        }
        existing->parent_ = parent;
        existing->source_index_ = source_index;
        existing->structural_path_ = std::move(structural_path);
        const auto& previous = *existing->description_;
        node_updated = node_updated || previous.source_path != description->source_path ||
                       previous.state_scope != description->state_scope;
        if (previous.source_path != description->source_path ||
            previous.state_scope != description->state_scope) {
            existing->mark_dirty(DirtyReason::properties);
            bump_dirty_generation(DirtyReason::properties);
        }
        if (previous.materialization_key != description->materialization_key) {
            existing->mark_dirty(DirtyReason::layout);
            bump_dirty_generation(DirtyReason::layout);
            node_layout_updated = true;
            node_updated = true;
        }
        if (!behaviors_equal(previous.behaviors, description->behaviors)) {
            existing->mark_dirty(DirtyReason::input);
            existing->mark_dirty(DirtyReason::semantics);
            bump_dirty_generation(DirtyReason::input);
            bump_dirty_generation(DirtyReason::semantics);
            node_updated = true;
        }
        const bool sequence_changed =
            (previous.virtual_sequence == nullptr) !=
                (description->virtual_sequence == nullptr) ||
            (previous.virtual_sequence != nullptr &&
             !previous.virtual_sequence->same_generation(*description->virtual_sequence));
        const bool virtual_metadata_changed =
            (previous.virtual_item_members == nullptr) !=
                (description->virtual_item_members == nullptr) ||
            (previous.virtual_item_members != nullptr &&
             *previous.virtual_item_members != *description->virtual_item_members) ||
            (previous.virtual_item_extents == nullptr) !=
                (description->virtual_item_extents == nullptr) ||
            (previous.virtual_item_extents != nullptr &&
             *previous.virtual_item_extents != *description->virtual_item_extents);
        if (sequence_changed || virtual_metadata_changed) {
            existing->mark_dirty(DirtyReason::layout);
            bump_dirty_generation(DirtyReason::layout);
            node_layout_updated = true;
            node_updated = true;
        }
        for (const auto& [name, next] : description->properties) {
            const auto current = previous.properties.find(name);
            if (current == previous.properties.end() ||
                !expression_value_equal(current->second, next)) {
                const DirtyReason reason = property_reason(
                    name,
                    current != previous.properties.end() ? &current->second : nullptr,
                    &next
                );
                existing->mark_dirty(reason);
                bump_dirty_generation(reason);
                node_layout_updated = node_layout_updated || affects_layout(reason);
                node_updated = true;
            }
        }
        for (const auto& [name, current] : previous.properties) {
            static_cast<void>(current);
            if (!description->properties.contains(name)) {
                const DirtyReason reason = property_reason(name, &current, nullptr);
                existing->mark_dirty(reason);
                bump_dirty_generation(reason);
                node_layout_updated = node_layout_updated || affects_layout(reason);
                node_updated = true;
            }
        }
        const auto previous_persistence = previous.properties.find("persistenceKey");
        const auto next_persistence = description->properties.find("persistenceKey");
        const bool persistence_changed =
            (previous_persistence == previous.properties.end()) !=
                (next_persistence == description->properties.end()) ||
            (previous_persistence != previous.properties.end() &&
             next_persistence != description->properties.end() &&
             !expression_value_equal(previous_persistence->second, next_persistence->second));
        existing->description_ = description;
        if (persistence_changed && persistence_fields_) {
            for (const std::string& field : persistence_fields_(description->type)) {
                existing->retained_values_.erase(field);
            }
            hydrate_persistence(*existing);
        }
    }

    if (!description->materialization.has_value() && existing->realized_range_.has_value()) {
        existing->realized_range_.reset();
        existing->realization_provider_.reset();
        existing->realization_theme_.reset();
        existing->realization_theme_scope_.reset();
        existing->realization_theme_generation_ = 0U;
        existing->realization_cache_.clear();
        existing->realization_cache_order_.clear();
        existing->warm_realization_state_scopes_.clear();
    }

    // A lazy description owns the provider and immutable collection properties; its currently
    // realized children belong to the retained collection node. An ordinary application/theme
    // reconcile must update the owner without interpreting the authored empty range as a request
    // to detach the live viewport. Surface refreshes the same local window against a changed
    // provider or theme generation before the next layout pass.
    if (description->materialization.has_value() && existing->realized_range_.has_value()) {
        if (node_layout_updated) {
            if (compatible) {
                if (existing->revision_ == std::numeric_limits<std::uint64_t>::max()) {
                    throw std::overflow_error("retained node revision exhausted");
                }
                ++existing->revision_;
            }
            layout_invalidated = true;
        }
        if (compatible && node_updated) ++stats.updated;
        existing->refresh_subtree_metadata();
        return existing;
    }

    const std::size_t child_count = description->children->size();
    MaterializationRange range{0U, child_count};
    if (description->materialization.has_value()) {
        range.start = std::min(description->materialization->start, child_count);
        range.end_exclusive = std::clamp(
            description->materialization->end_exclusive, range.start, child_count
        );
    }

    std::vector<std::uint64_t> previous_order;
    std::map<std::uint64_t, std::unique_ptr<RetainedNode>> available;
    std::map<SiblingKey, std::uint64_t> keyed;
    std::map<std::size_t, std::uint64_t> positional;
    previous_order.reserve(existing->children_.size());
    for (auto& child : existing->children_) {
        const std::uint64_t identity = child->identity_;
        previous_order.push_back(identity);
        if (child->description_->key.has_value()) {
            keyed.emplace(
                SiblingKey{child->description_->type, *child->description_->key}, identity
            );
        } else {
            positional.emplace(child->source_index_, identity);
        }
        available.emplace(identity, std::move(child));
    }
    existing->children_.clear();

    std::set<SiblingKey> next_keys;
    for (std::size_t index = range.start; index < range.end_exclusive; ++index) {
        std::shared_ptr<const DescriptionNode> child_description = description->children->at(index);
        ++stats.materialized;
        std::optional<std::uint64_t> matched_identity;
        if (child_description->key.has_value()) {
            const SiblingKey key{child_description->type, *child_description->key};
            if (!next_keys.insert(key).second) {
                throw std::invalid_argument(
                    "description siblings contain duplicate key '" + *child_description->key +
                    "' for type '" + child_description->type + "'"
                );
            }
            if (const auto found = keyed.find(key); found != keyed.end()) {
                matched_identity = found->second;
                keyed.erase(found);
            }
        } else if (const auto found = positional.find(index); found != positional.end()) {
            const auto candidate = available.find(found->second);
            if (candidate != available.end() &&
                candidate->second->description_->type == child_description->type) {
                matched_identity = found->second;
                positional.erase(found);
            }
        }
        std::unique_ptr<RetainedNode> child_existing;
        if (matched_identity.has_value()) {
            const auto found = available.find(*matched_identity);
            child_existing = std::move(found->second);
            available.erase(found);
        }
        bool child_layout_invalidated = false;
        existing->children_.push_back(reconcile_node(
            std::move(child_existing),
            std::move(child_description),
            existing.get(),
            index,
            child_path(existing->structural_path_, index),
            stats,
            child_layout_invalidated,
            exit_retention
        ));
        node_layout_updated = node_layout_updated || child_layout_invalidated;
    }

    bool removed_children = false;
    for (const std::uint64_t identity : previous_order) {
        const auto found = available.find(identity);
        if (found == available.end()) continue;
        std::unique_ptr<RetainedNode> removed = std::move(found->second);
        available.erase(found);
        const bool already_exiting = removed->lifecycle_ == RetainedLifecycle::exiting;
        const bool retain = already_exiting ||
                            (exit_retention && exit_retention(*removed));
        if (!retain) {
            removed_children = true;
            detach(std::move(removed), &stats);
            continue;
        }
        if (!already_exiting) {
            removed_children = true;
            mark_subtree_lifecycle(*removed, RetainedLifecycle::exiting);
            removed->mark_dirty(DirtyReason::animation);
            bump_dirty_generation(DirtyReason::animation);
        }
        const std::size_t retained_index = existing->children_.size();
        rebase_subtree(
            *removed,
            existing.get(),
            retained_index,
            child_path(existing->structural_path_, retained_index)
        );
        existing->children_.push_back(std::move(removed));
    }

    std::vector<std::uint64_t> next_order;
    next_order.reserve(existing->children_.size());
    for (const auto& child : existing->children_) next_order.push_back(child->identity_);
    if (removed_children || (compatible && previous_order != next_order)) {
        existing->mark_dirty(DirtyReason::structure);
        bump_dirty_generation(DirtyReason::structure);
        node_layout_updated = true;
        node_updated = true;
    }
    if (node_layout_updated) {
        if (compatible) {
            if (existing->revision_ == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("retained node revision exhausted");
            }
            ++existing->revision_;
        }
        layout_invalidated = true;
    }
    if (compatible && node_updated) ++stats.updated;
    existing->refresh_subtree_metadata();
    return existing;
}

ReconcileStats RetainedTree::realize_children(
    const std::uint64_t parent_identity,
    std::shared_ptr<const DescriptionChildren> provider,
    std::shared_ptr<const Theme> projected_theme,
    std::optional<std::string> projected_theme_scope,
    const std::uint64_t theme_generation,
    MaterializationRange range,
    std::vector<RealizedDescriptionChild> children,
    const ExitRetention& exit_retention
) {
    if (provider == nullptr) {
        throw std::invalid_argument("virtual realization requires a description provider");
    }
    RetainedNode* const parent = find_identity(parent_identity);
    if (parent == nullptr || parent->lifecycle_ != RetainedLifecycle::attached) {
        throw std::invalid_argument("virtual realization parent is not attached");
    }
    if (!parent->description_->materialization.has_value()) {
        throw std::invalid_argument("virtual realization parent is not a lazy collection");
    }
    const std::size_t child_count = provider->size();
    range.start = std::min(range.start, child_count);
    range.end_exclusive = std::clamp(range.end_exclusive, range.start, child_count);
    if (children.size() != range.end_exclusive - range.start) {
        throw std::invalid_argument("virtual realization rows do not cover the requested range");
    }
    for (std::size_t offset = 0U; offset < children.size(); ++offset) {
        if (children[offset].source_index != range.start + offset ||
            children[offset].description == nullptr) {
            throw std::invalid_argument(
                "virtual realization rows must be non-null and ordered by source index"
            );
        }
    }
    std::set<SiblingKey> desired_keys;
    for (const RealizedDescriptionChild& child : children) {
        if (!child.description->key.has_value()) continue;
        const SiblingKey key{child.description->type, *child.description->key};
        if (!desired_keys.insert(key).second) {
            throw std::invalid_argument(
                "virtual collection contains duplicate key '" +
                *child.description->key + "' for type '" +
                child.description->type + "'"
            );
        }
    }

    ReconcileStats stats;
    bool layout_invalidated = false;
    const bool same_generation =
        parent->realization_provider_ == provider &&
        parent->realization_theme_ == projected_theme &&
        parent->realization_theme_scope_ == projected_theme_scope &&
        parent->realization_theme_generation_ == theme_generation;
    if (!same_generation) {
        parent->realization_cache_.clear();
        parent->realization_cache_order_.clear();
        parent->warm_realization_state_scopes_.clear();
    }

    std::vector<std::uint64_t> previous_order;
    std::map<std::uint64_t, std::unique_ptr<RetainedNode>> available;
    std::map<SiblingKey, std::uint64_t> keyed;
    std::map<std::size_t, std::uint64_t> positional;
    previous_order.reserve(parent->children_.size());
    for (auto& child : parent->children_) {
        const std::uint64_t identity = child->identity_;
        previous_order.push_back(identity);
        if (child->description_->key.has_value()) {
            keyed.emplace(
                SiblingKey{child->description_->type, *child->description_->key}, identity
            );
        } else {
            positional.emplace(child->source_index_, identity);
        }
        available.emplace(identity, std::move(child));
    }
    parent->children_.clear();

    for (RealizedDescriptionChild& realized : children) {
        ++stats.materialized;
        parent->realization_cache_.erase(realized.source_index);
        std::erase(parent->realization_cache_order_, realized.source_index);
        std::optional<std::uint64_t> matched_identity;
        if (realized.description->key.has_value()) {
            const SiblingKey key{
                realized.description->type,
                *realized.description->key,
            };
            if (const auto found = keyed.find(key); found != keyed.end()) {
                matched_identity = found->second;
                keyed.erase(found);
            }
        } else if (const auto found = positional.find(realized.source_index);
                   found != positional.end()) {
            const auto candidate = available.find(found->second);
            if (candidate != available.end() &&
                candidate->second->description_->type == realized.description->type) {
                matched_identity = found->second;
                positional.erase(found);
            }
        }
        std::unique_ptr<RetainedNode> existing;
        if (matched_identity.has_value()) {
            const auto found = available.find(*matched_identity);
            existing = std::move(found->second);
            available.erase(found);
        }
        bool child_layout_invalidated = false;
        parent->children_.push_back(reconcile_node(
            std::move(existing),
            std::move(realized.description),
            parent,
            realized.source_index,
            child_path(parent->structural_path_, realized.source_index),
            stats,
            child_layout_invalidated,
            exit_retention
        ));
        layout_invalidated = layout_invalidated || child_layout_invalidated;
    }

    bool removed_children = false;
    for (const std::uint64_t identity : previous_order) {
        const auto found = available.find(identity);
        if (found == available.end()) continue;
        std::unique_ptr<RetainedNode> removed = std::move(found->second);
        available.erase(found);
        if (same_generation && removed->lifecycle_ == RetainedLifecycle::attached) {
            const std::size_t cached_index = removed->source_index_;
            parent->realization_cache_.insert_or_assign(
                cached_index,
                removed->description_
            );
            std::erase(parent->realization_cache_order_, cached_index);
            parent->realization_cache_order_.push_back(cached_index);
        }
        const bool already_exiting = removed->lifecycle_ == RetainedLifecycle::exiting;
        const bool retain = already_exiting ||
                            (exit_retention && exit_retention(*removed));
        if (!retain) {
            removed_children = true;
            detach(std::move(removed), &stats);
            continue;
        }
        if (!already_exiting) {
            removed_children = true;
            mark_subtree_lifecycle(*removed, RetainedLifecycle::exiting);
            removed->mark_dirty(DirtyReason::animation);
            bump_dirty_generation(DirtyReason::animation);
        }
        // An exiting virtual child retains its source coordinate so layout/motion can keep
        // interpreting it in the collection's extent space.
        rebase_subtree(
            *removed,
            parent,
            removed->source_index_,
            child_path(parent->structural_path_, removed->source_index_)
        );
        parent->children_.push_back(std::move(removed));
    }

    parent->realization_provider_ = std::move(provider);
    parent->realization_theme_ = std::move(projected_theme);
    parent->realization_theme_scope_ = std::move(projected_theme_scope);
    parent->realization_theme_generation_ = theme_generation;
    parent->realized_range_ = range;
    const std::size_t window = range.end_exclusive - range.start;
    const std::size_t cache_capacity = std::clamp(
        window > 64U ? std::size_t{256U} : window * 4U,
        std::size_t{32U},
        std::size_t{256U}
    );
    while (parent->realization_cache_order_.size() > cache_capacity) {
        const std::size_t expired = parent->realization_cache_order_.front();
        parent->realization_cache_order_.pop_front();
        parent->realization_cache_.erase(expired);
    }
    parent->warm_realization_state_scopes_.clear();
    for (const auto& [index, description] : parent->realization_cache_) {
        static_cast<void>(index);
        if (description->materialization_result == nullptr) continue;
        const runtime::StateScopeSet& scopes =
            description->materialization_result->owned_state_scopes;
        parent->warm_realization_state_scopes_.insert(scopes.begin(), scopes.end());
    }

    std::vector<std::uint64_t> next_order;
    next_order.reserve(parent->children_.size());
    for (const auto& child : parent->children_) next_order.push_back(child->identity_);
    const bool structure_changed = removed_children || previous_order != next_order;
    if (structure_changed) {
        static_cast<void>(mark(parent_identity, DirtyReason::structure));
        layout_invalidated = true;
        ++stats.updated;
    } else if (layout_invalidated) {
        static_cast<void>(mark(parent_identity, DirtyReason::layout));
    }

    for (RetainedNode* current = parent; current != nullptr; current = current->parent_) {
        current->refresh_subtree_metadata();
    }
    if (stats.changed()) {
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("retained tree generation exhausted");
        }
        ++generation_;
        invalidate_description_snapshot();
        rebuild_indexes();
    }
    stats.generation = generation_;
    return stats;
}

void RetainedTree::mark_subtree_lifecycle(
    RetainedNode& node,
    const RetainedLifecycle lifecycle
) noexcept {
    node.lifecycle_ = lifecycle;
    for (auto& child : node.children_) mark_subtree_lifecycle(*child, lifecycle);
    node.refresh_subtree_metadata();
}

void RetainedTree::rebase_subtree(
    RetainedNode& node,
    RetainedNode* const parent,
    const std::size_t source_index,
    std::string structural_path
) {
    node.parent_ = parent;
    node.source_index_ = source_index;
    node.structural_path_ = std::move(structural_path);
    for (std::size_t index = 0U; index < node.children_.size(); ++index) {
        rebase_subtree(
            *node.children_[index],
            &node,
            index,
            child_path(node.structural_path_, index)
        );
    }
}

std::size_t RetainedTree::prune_exiting(const ExitCompletion& exit_completion) {
    if (!exit_completion || root_ == nullptr) return 0U;
    std::size_t detached = 0U;
    const auto prune = [&](auto&& self, RetainedNode& parent) -> void {
        // An EXITING child is one retained lifecycle boundary. Do not prune eligible
        // descendants first: doing so can erase the only exit attachment that justified
        // retaining the ancestor and strand a hollow EXITING subtree forever.
        for (auto& child : parent.children_) {
            if (child->lifecycle_ != RetainedLifecycle::exiting) self(self, *child);
        }
        const bool changed = std::ranges::any_of(
            parent.children_,
            [&exit_completion](const std::unique_ptr<RetainedNode>& child) {
                return child->lifecycle_ == RetainedLifecycle::exiting &&
                       exit_completion(*child);
            }
        );
        if (changed) {
            std::vector<std::unique_ptr<RetainedNode>> retained;
            retained.reserve(parent.children_.size());
            for (auto& child : parent.children_) {
                if (child->lifecycle_ == RetainedLifecycle::exiting && exit_completion(*child)) {
                    ReconcileStats stats;
                    detach(std::move(child), &stats);
                    detached += stats.detached;
                } else {
                    retained.push_back(std::move(child));
                }
            }
            parent.children_ = std::move(retained);
            // Use the ordinary dirty path so every ancestor revision changes. Measurement cache
            // entries contain retained-node pointers for their complete measured subtrees; bumping
            // only the immediate parent would let an ancestor return a stale, dangling snapshot.
            static_cast<void>(mark(parent.identity_, DirtyReason::structure));
        }
        // Descendant pruning also changes this aggregate even when no immediate child was removed.
        parent.refresh_subtree_metadata();
    };
    prune(prune, *root_);
    if (detached == 0U) return 0U;
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("retained tree generation exhausted");
    }
    ++generation_;
    invalidate_description_snapshot();
    rebuild_indexes();
    return detached;
}

} // namespace strata::ui
