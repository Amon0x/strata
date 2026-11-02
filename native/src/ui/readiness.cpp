#include "ui/readiness.hpp"

#include <algorithm>
#include <utility>

#include "ui/input.hpp"
#include "ui/tree.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
namespace {

void observe_read(std::vector<base::ReadinessRead>& reads, const base::ReadinessRead& read) {
    if (read.resource.id() == 0U)
        return;
    const auto found = std::ranges::find(reads, read.resource, &base::ReadinessRead::resource);
    if (found == reads.end())
        reads.push_back(read);
    else
        *found = read;
}

[[nodiscard]] bool watching(const std::vector<base::ReadinessRead>& reads) {
    return std::ranges::any_of(reads, &base::ReadinessRead::watching);
}

} // namespace

void PreparationReads::observe(const std::uint64_t node, const base::Readiness& resource) {
    const base::ReadinessRead read{resource, resource.status()};
    observe(node, std::span(&read, 1U));
}

void PreparationReads::observe(const std::uint64_t node,
                               const std::span<const base::ReadinessRead> resources) {
    if (resources.empty())
        return;
    if (active_ != nullptr) {
        for (const auto& resource : resources)
            observe_read(active_->reads_, resource);
        return;
    }
    // Input helpers can request a layout outside a cached paint/measurement operation. Keep only
    // unresolved reads: adoption invalidates their text geometry, then releases these observations.
    const Key key{node, PreparationUse::input};
    for (const auto& resource : resources) {
        if (resource.resource.id() != 0U && resource.observed == base::PendingStatus::pending) {
            watched_.insert(key);
            observe_read(reads_[key].resources, resource);
        }
    }
}

void PreparationReads::replace(const Key key, std::vector<base::ReadinessRead> reads,
                               const std::optional<WidgetFrameCost> frame_cost) {
    if (reads.empty()) {
        reads_.erase(key);
        watched_.erase(key);
        return;
    }
    const bool waiting = watching(reads);
    if (waiting)
        watched_.insert(key);
    reads_.insert_or_assign(key, Observation{std::move(reads), frame_cost});
    if (!waiting)
        watched_.erase(key);
}

void PreparationReads::erase(const std::uint64_t node) noexcept {
    const Key first{node, PreparationUse::measurement};
    const Key last{node, PreparationUse::input};
    reads_.erase(reads_.lower_bound(first), reads_.upper_bound(last));
    watched_.erase(watched_.lower_bound(first), watched_.upper_bound(last));
}

void PreparationReads::erase(const std::uint64_t node, const PreparationUse use) noexcept {
    reads_.erase({node, use});
    watched_.erase({node, use});
}

bool PreparationReads::references(const base::Readiness& resource) const noexcept {
    return std::ranges::any_of(reads_, [&resource](const auto& entry) {
        return std::ranges::any_of(entry.second.resources, [&resource](const base::ReadinessRead& read) {
            return read.resource == resource;
        });
    });
}

void PreparationReads::invalidate(RetainedTree& tree, InputRouter* const input) {
    for (auto current = watched_.begin(); current != watched_.end();) {
        const auto found = reads_.find(*current);
        if (found == reads_.end()) {
            current = watched_.erase(current);
            continue;
        }
        const bool changed =
            !std::ranges::all_of(found->second.resources, &base::ReadinessRead::current);
        if (!changed) {
            ++current;
            continue;
        }
        const bool text = std::ranges::any_of(found->second.resources, [](const base::ReadinessRead& read) {
            return !read.current() && (read.resource.kind() == base::TaskKind::font ||
                                      read.resource.kind() == base::TaskKind::shaping);
        });
        const PreparationUse use = current->second;
        if (use != PreparationUse::frame || text) {
            static_cast<void>(tree.mark(current->first,
                text || use == PreparationUse::input ? DirtyReason::text
                : use == PreparationUse::measurement ? DirtyReason::layout : DirtyReason::paint));
        }
        if (use == PreparationUse::frame && input != nullptr) {
            if (const RetainedNode* node = tree.find_identity(current->first); node != nullptr)
                static_cast<void>(input->resume_widget_frame(*node, found->second.frame_cost));
        }
        current = watched_.erase(current);
        if (use == PreparationUse::input)
            reads_.erase(found);
    }
}

NodeReadinessSnapshot PreparationReads::snapshot(const std::uint64_t node) const {
    NodeReadinessSnapshot result;
    for (auto current = reads_.lower_bound({node, PreparationUse::measurement});
         current != reads_.end() && current->first.first == node; ++current) {
        const PreparationUse use = current->first.second;
        const ReadinessPhase phase =
            use == PreparationUse::measurement || use == PreparationUse::input
                ? ReadinessPhase::layout
                : use == PreparationUse::frame ? ReadinessPhase::frame : ReadinessPhase::paint;
        for (const auto& read : current->second.resources)
            result.push_back(NodeReadinessRead{node, read, phase});
    }
    std::ranges::sort(result, {}, [](const NodeReadinessRead& read) {
        return std::pair(read.dependency.resource.id(), read.phase);
    });
    result.erase(std::ranges::unique(result, [](const auto& left, const auto& right) {
        return left.phase == right.phase && left.dependency == right.dependency;
    }).begin(), result.end());
    return result;
}

PreparationReadScope::PreparationReadScope(const RetainedNode& node, const PreparationUse use,
                                           const std::optional<WidgetFrameCost> frame_cost)
    : owner_(*node.preparation_reads_), key_(node.identity(), use), previous_(owner_.active_),
      frame_cost_(frame_cost) {
    owner_.active_ = this;
}

PreparationReadScope::~PreparationReadScope() {
    if (active_)
        owner_.active_ = previous_;
}

void PreparationReadScope::commit() {
    owner_.active_ = previous_;
    active_ = false;
    owner_.replace(key_, std::move(reads_), frame_cost_);
}

} // namespace strata::ui
