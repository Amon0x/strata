#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "runtime/sequence.hpp"

namespace strata::ui::collection {

/** Immutable item extents with a prefix table for logarithmic viewport/index resolution. */
class VirtualItemExtents final {
public:
    explicit VirtualItemExtents(std::vector<double> values);

    [[nodiscard]] const std::vector<double>& values() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] double total() const noexcept;
    [[nodiscard]] double start(std::size_t index) const;
    [[nodiscard]] double extent(std::size_t index) const;
    [[nodiscard]] std::size_t first_index_ending_after(double position) const noexcept;
    [[nodiscard]] std::size_t end_index_starting_before(double position) const noexcept;
    [[nodiscard]] std::optional<std::size_t> index_at(double position) const noexcept;

    [[nodiscard]] friend bool operator==(
        const VirtualItemExtents&,
        const VirtualItemExtents&
    ) = default;

private:
    std::vector<double> values_;
    std::vector<double> prefixes_;
};

struct VirtualMeasurement final {
    std::size_t index = 0U;
    std::string key;
    double extent = 0.0;
};

struct VirtualExtentRequest final {
    std::shared_ptr<const runtime::KeyedSequence> items;
    double estimated_extent = 0.0;
    std::optional<VirtualItemExtents> authored_extents;
    bool measure_item_extents = false;
    bool reset_anchor_on_change = false;
};

struct VirtualExtentResolution final {
    VirtualItemExtents extents;
    double offset = 0.0;
    bool anchor_changed = false;
};

struct VirtualAnchorUpdate final {
    std::uint64_t identity = 0U;
    double x = 0.0;
    double y = 0.0;
};

/**
 * Layout-engine-owned measurement state for virtual collections. Measurements are stored by
 * domain key so insertions and reorders retain both learned extents and the viewport anchor.
 */
class VirtualizationCache final {
public:
    [[nodiscard]] VirtualExtentResolution resolve(
        std::uint64_t identity,
        const VirtualExtentRequest& request,
        double offset,
        std::span<const VirtualMeasurement> measurements
    );
    void queue_anchor(VirtualAnchorUpdate update);
    [[nodiscard]] std::vector<VirtualAnchorUpdate> take_anchor_updates();
    void retain(const std::set<std::uint64_t>& identities);
    void clear() noexcept;

private:
    struct Entry final {
        std::shared_ptr<const runtime::KeyedSequence> items;
        std::map<std::string, double, std::less<>> measured_by_key;
        std::optional<VirtualItemExtents> extents;
        std::optional<VirtualItemExtents> authored_extents;
        double estimated_extent = 0.0;
        bool measure_item_extents = false;
    };

    std::map<std::uint64_t, Entry> entries_;
    std::map<std::uint64_t, VirtualAnchorUpdate> pending_anchor_updates_;
};

} // namespace strata::ui::collection
