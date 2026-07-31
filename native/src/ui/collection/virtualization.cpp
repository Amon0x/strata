#include "ui/collection/virtualization.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace strata::ui::collection {
namespace {

constexpr double measurement_epsilon = 0.01;

[[nodiscard]] bool near(const double left, const double right) noexcept {
    return std::abs(left - right) <= measurement_epsilon;
}

} // namespace

VirtualItemExtents::VirtualItemExtents(std::vector<double> values)
    : values_(std::move(values)), prefixes_(values_.size() + 1U, 0.0) {
    for (std::size_t index = 0U; index < values_.size(); ++index) {
        const double extent = values_[index];
        if (!std::isfinite(extent) || extent <= 0.0) {
            throw std::invalid_argument("virtual item extents must be finite and positive");
        }
        prefixes_[index + 1U] = prefixes_[index] + extent;
        if (!std::isfinite(prefixes_[index + 1U])) {
            throw std::overflow_error("virtual item extent prefix overflow");
        }
    }
}

const std::vector<double>& VirtualItemExtents::values() const noexcept { return values_; }
std::size_t VirtualItemExtents::size() const noexcept { return values_.size(); }
double VirtualItemExtents::total() const noexcept { return prefixes_.back(); }

double VirtualItemExtents::start(const std::size_t index) const {
    if (index >= values_.size()) throw std::out_of_range("virtual item index is outside the prefix table");
    return prefixes_[index];
}

double VirtualItemExtents::extent(const std::size_t index) const {
    if (index >= values_.size()) throw std::out_of_range("virtual item index is outside the extent table");
    return values_[index];
}

std::size_t VirtualItemExtents::first_index_ending_after(const double position) const noexcept {
    const auto found = std::upper_bound(prefixes_.begin() + 1, prefixes_.end(), position);
    return static_cast<std::size_t>(found - (prefixes_.begin() + 1));
}

std::size_t VirtualItemExtents::end_index_starting_before(const double position) const noexcept {
    const auto found = std::lower_bound(prefixes_.begin(), prefixes_.end() - 1, position);
    return static_cast<std::size_t>(found - prefixes_.begin());
}

std::optional<std::size_t> VirtualItemExtents::index_at(const double position) const noexcept {
    if (values_.empty() || !std::isfinite(position) || position < 0.0 || position >= total()) {
        return std::nullopt;
    }
    const std::size_t index = first_index_ending_after(position);
    return index < values_.size() ? std::optional<std::size_t>(index) : std::nullopt;
}

VirtualExtentResolution VirtualizationCache::resolve(
    const std::uint64_t identity,
    const VirtualExtentRequest& request,
    const double offset,
    const std::span<const VirtualMeasurement> measurements
) {
    if (request.items == nullptr) {
        throw std::invalid_argument("virtual extent resolution requires a keyed sequence");
    }
    if (!std::isfinite(request.estimated_extent) || request.estimated_extent <= 0.0) {
        throw std::invalid_argument("virtual item estimate must be finite and positive");
    }
    if (request.authored_extents.has_value() &&
        request.authored_extents->size() != request.items->count()) {
        throw std::invalid_argument("authored virtual item extents must match the item count");
    }

    Entry& entry = entries_[identity];
    bool changed = entry.items == nullptr ||
                   !entry.items->same_generation(*request.items) ||
                   !entry.extents.has_value() ||
                   entry.authored_extents != request.authored_extents ||
                   !near(entry.estimated_extent, request.estimated_extent) ||
                   entry.measure_item_extents != request.measure_item_extents;

    if (request.measure_item_extents) {
        for (const VirtualMeasurement& measurement : measurements) {
            if (measurement.index >= request.items->count() ||
                !std::isfinite(measurement.extent) ||
                measurement.extent <= 0.0) {
                continue;
            }
            const std::string key = !measurement.key.empty()
                                        ? measurement.key
                                        : request.items->key_at(measurement.index);
            const auto found = entry.measured_by_key.find(key);
            if (found == entry.measured_by_key.end() || !near(found->second, measurement.extent)) {
                entry.measured_by_key.insert_or_assign(key, measurement.extent);
                changed = true;
            }
        }
    }

    std::optional<std::size_t> old_anchor;
    double within_anchor = 0.0;
    std::string anchor_key;
    if (changed && !request.reset_anchor_on_change && entry.extents.has_value() && entry.items != nullptr &&
        entry.items->count() != 0U &&
        std::isfinite(offset) && offset >= 0.0 && entry.extents->total() > 0.0) {
        const double probe = std::min(offset, std::max(0.0, entry.extents->total() - 0.001));
        old_anchor = entry.extents->index_at(probe);
        if (old_anchor.has_value() && *old_anchor < entry.items->count()) {
            anchor_key = entry.items->key_at(*old_anchor);
            within_anchor = std::max(0.0, offset - entry.extents->start(*old_anchor));
        }
    }

    if (changed) {
        std::erase_if(entry.measured_by_key, [&request](const auto& value) {
            return !request.items->index_of_key(value.first).has_value();
        });
        std::vector<double> values = request.authored_extents.has_value()
                                         ? request.authored_extents->values()
                                         : std::vector<double>(
                                               request.items->count(),
                                               request.estimated_extent
                                           );
        if (request.measure_item_extents) {
            for (const auto& [key, measured] : entry.measured_by_key) {
                if (const std::optional<std::size_t> index =
                        request.items->index_of_key(key);
                    index.has_value()) {
                    values[*index] = measured;
                }
            }
        }
        entry.items = request.items;
        entry.extents.emplace(std::move(values));
        entry.authored_extents = request.authored_extents;
        entry.estimated_extent = request.estimated_extent;
        entry.measure_item_extents = request.measure_item_extents;
    }

    double resolved_offset = changed && request.reset_anchor_on_change
        ? 0.0
        : std::isfinite(offset) ? std::max(0.0, offset) : 0.0;
    bool anchor_changed = changed && request.reset_anchor_on_change && !near(resolved_offset, offset);
    if (!anchor_key.empty()) {
        if (const std::optional<std::size_t> next =
                entry.items->index_of_key(anchor_key);
            next.has_value()) {
            resolved_offset = entry.extents->start(*next) +
                std::min(within_anchor, entry.extents->extent(*next));
            anchor_changed = !near(resolved_offset, offset);
        }
    }
    return VirtualExtentResolution{*entry.extents, resolved_offset, anchor_changed};
}

void VirtualizationCache::queue_anchor(VirtualAnchorUpdate update) {
    pending_anchor_updates_.insert_or_assign(update.identity, std::move(update));
}

std::vector<VirtualAnchorUpdate> VirtualizationCache::take_anchor_updates() {
    std::vector<VirtualAnchorUpdate> result;
    result.reserve(pending_anchor_updates_.size());
    for (auto& [identity, update] : pending_anchor_updates_) {
        static_cast<void>(identity);
        result.push_back(std::move(update));
    }
    pending_anchor_updates_.clear();
    return result;
}

void VirtualizationCache::retain(const std::set<std::uint64_t>& identities) {
    std::erase_if(entries_, [&identities](const auto& value) {
        return !identities.contains(value.first);
    });
    std::erase_if(pending_anchor_updates_, [&identities](const auto& value) {
        return !identities.contains(value.first);
    });
}

void VirtualizationCache::clear() noexcept {
    entries_.clear();
    pending_anchor_updates_.clear();
}

} // namespace strata::ui::collection
