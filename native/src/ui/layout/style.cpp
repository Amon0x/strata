#include "ui/layout.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/layout/detail_algorithms.hpp"

namespace strata::ui {
using namespace layout_detail;

LayoutSize LayoutSize::clamp(
    std::optional<LayoutSize> minimum,
    LayoutSize preferred,
    std::optional<LayoutSize> maximum
) {
    LayoutSize result(Kind::clamp);
    if (minimum.has_value()) result.minimum = std::make_shared<const LayoutSize>(std::move(*minimum));
    result.preferred = std::make_shared<const LayoutSize>(std::move(preferred));
    if (maximum.has_value()) result.maximum = std::make_shared<const LayoutSize>(std::move(*maximum));
    return result;
}

bool operator==(const LayoutSize& left, const LayoutSize& right) {
    const auto equal = [](const auto& first, const auto& second) {
        return first == second || (first != nullptr && second != nullptr && *first == *second);
    };
    return left.kind == right.kind && left.value == right.value &&
           equal(left.minimum, right.minimum) && equal(left.preferred, right.preferred) &&
           equal(left.maximum, right.maximum);
}
namespace {

class IndexKeyedSequence final : public runtime::KeyedSequence {
public:
    explicit IndexKeyedSequence(const std::size_t count) : count_(count) {}

    [[nodiscard]] std::uint64_t generation() const noexcept override {
        return static_cast<std::uint64_t>(count_);
    }
    [[nodiscard]] std::size_t count() const noexcept override { return count_; }
    [[nodiscard]] std::string key_at(const std::size_t index) const override {
        if (index >= count_) throw std::out_of_range("virtual key index is outside the sequence");
        return "$index:" + std::to_string(index);
    }
    [[nodiscard]] std::optional<std::size_t> index_of_key(
        const std::string_view key
    ) const override {
        constexpr std::string_view prefix = "$index:";
        if (!key.starts_with(prefix)) return std::nullopt;
        std::size_t index = 0U;
        const char* const begin = key.data() + static_cast<std::ptrdiff_t>(prefix.size());
        const char* const end = key.data() + static_cast<std::ptrdiff_t>(key.size());
        const auto parsed = std::from_chars(begin, end, index);
        return parsed.ec == std::errc{} && parsed.ptr == end && index < count_
                   ? std::optional<std::size_t>(index)
                   : std::nullopt;
    }
    [[nodiscard]] bool same_generation(
        const runtime::KeyedSequence& other
    ) const noexcept override {
        const auto* indexed = dynamic_cast<const IndexKeyedSequence*>(&other);
        return indexed != nullptr && indexed->count_ == count_;
    }

private:
    std::size_t count_;
};

class SnapshotKeyedSequence final : public runtime::KeyedSequence {
public:
    explicit SnapshotKeyedSequence(std::vector<std::string> keys) : keys_(std::move(keys)) {}

    [[nodiscard]] std::uint64_t generation() const noexcept override { return 0U; }
    [[nodiscard]] std::size_t count() const noexcept override { return keys_.size(); }
    [[nodiscard]] std::string key_at(const std::size_t index) const override {
        return keys_.at(index);
    }
    [[nodiscard]] std::optional<std::size_t> index_of_key(
        const std::string_view key
    ) const override {
        const auto found = std::ranges::find(keys_, key);
        return found != keys_.end()
                   ? std::optional<std::size_t>(static_cast<std::size_t>(found - keys_.begin()))
                   : std::nullopt;
    }
    [[nodiscard]] bool same_generation(
        const runtime::KeyedSequence& other
    ) const noexcept override {
        const auto* snapshot = dynamic_cast<const SnapshotKeyedSequence*>(&other);
        return snapshot != nullptr && snapshot->keys_ == keys_;
    }

private:
    std::vector<std::string> keys_;
};

} // namespace

std::size_t VirtualListSpec::item_count() const noexcept {
    return items != nullptr ? items->count() : 0U;
}

double VirtualListSpec::total_item_extent() const noexcept {
    return item_extents.has_value()
               ? item_extents->total()
               : item_extent * static_cast<double>(item_count());
}

double VirtualListSpec::item_start(const std::size_t index) const {
    return item_extents.has_value()
               ? item_extents->start(index)
               : item_extent * static_cast<double>(index);
}

double VirtualListSpec::extent_at(const std::size_t index) const {
    return item_extents.has_value() ? item_extents->extent(index) : item_extent;
}

LayoutStyle layout_style(const DescriptionNode& description) {
    LayoutStyle style;
    style.participates = boolean(scalar_property(description, "$layoutParticipates"), true);
    const runtime::Value* layout = layout_value(description);
    style.kind = kind(text(field(layout, "kind")));
    style.width = layout_size(field(layout, "width"));
    style.height = layout_size(field(layout, "height"));
    if (const runtime::Value* value = field(layout, "minWidth"); value != nullptr) style.min_width = layout_size(value);
    if (const runtime::Value* value = field(layout, "minHeight"); value != nullptr) style.min_height = layout_size(value);
    if (const runtime::Value* value = field(layout, "maxWidth"); value != nullptr) style.max_width = layout_size(value);
    if (const runtime::Value* value = field(layout, "maxHeight"); value != nullptr) style.max_height = layout_size(value);
    const double aspect = number(field(layout, "aspectRatio"));
    if (std::isfinite(aspect) && aspect > 0.0) style.aspect_ratio = aspect;
    if (const runtime::Value* intrinsic_value = field(layout, "intrinsicSize");
        intrinsic_value != nullptr && intrinsic_value->object() != nullptr) {
        const double width = non_negative_number(intrinsic_value->field("width"));
        const double height = non_negative_number(intrinsic_value->field("height"));
        style.intrinsic_size = Size{width, height};
    }
    style.padding = edges(field(layout, "padding"));
    style.margin = edges(field(layout, "margin"));
    const Edges gap = edges(field(layout, "gap"));
    style.gap = Point{gap.horizontal() * 0.5, gap.vertical() * 0.5};
    style.align_items = align(text(field(layout, "alignItems")));
    style.justify_content_authored = field(layout, "justifyContent") != nullptr;
    style.justify_content = justify(text(field(layout, "justifyContent")));
    style.align_content = justify(text(field(layout, "alignContent")));
    if (const runtime::Value* value = field(layout, "alignSelf"); value != nullptr && value->string() != nullptr) {
        style.align_self = align(*value->string());
    }
    if (const runtime::Value* value = field(layout, "justifySelf"); value != nullptr && value->string() != nullptr) {
        style.justify_self = align(*value->string());
    }
    if (const runtime::Value* value = field(layout, "placement");
        value != nullptr && value->object() != nullptr) {
        LayoutPlacement placement;
        if (const runtime::Value* x = value->field("x"); x != nullptr) {
            placement.x = layout_size(x);
        }
        if (const runtime::Value* y = value->field("y"); y != nullptr) {
            placement.y = layout_size(y);
        }
        placement.anchor_x = number(value->field("anchorX"));
        placement.anchor_y = number(value->field("anchorY"));
        placement.offset_x = number(value->field("offsetX"));
        placement.offset_y = number(value->field("offsetY"));
        style.placement = std::move(placement);
    }
    style.wrap = boolean(field(layout, "wrap"));
    style.clip = boolean(field(layout, "clip"));
    const runtime::Value* columns = field(layout, "columns");
    if (columns == nullptr) columns = scalar_property(description, "columns");
    const runtime::Value* rows = field(layout, "rows");
    if (rows == nullptr) rows = scalar_property(description, "rows");
    style.grid_columns = layout_tracks(columns);
    style.grid_rows = layout_tracks(rows);
    style.grid_column = index_value(field(layout, "gridColumn"));
    style.grid_row = index_value(field(layout, "gridRow"));
    style.column_span = span_value(field(layout, "columnSpan"));
    style.row_span = span_value(field(layout, "rowSpan"));
    const double z = number(field(layout, "zIndex"));
    if (std::isfinite(z)) {
        const double bounded = std::clamp(
            std::trunc(z),
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max())
        );
        style.z_index = static_cast<int>(bounded);
    }
    if (style.kind == LayoutKind::scroll) {
        style.scroll_horizontal = boolean(field(layout, "scrollHorizontal"), false);
        style.scroll_vertical = boolean(field(layout, "scrollVertical"), !style.scroll_horizontal);
        if (!style.scroll_horizontal && !style.scroll_vertical) style.scroll_vertical = true;
        style.scroll_viewport_insets = edges(field(layout, "viewportInsets"));
        style.scroll_viewport_insets_from_inside_border =
            boolean(field(layout, "viewportInsetsFromInsideBorder"));
        if (style.scroll_viewport_insets_from_inside_border) {
            const runtime::Value* border = resolved_style_property(
                description, layout, "border"
            );
            if (border != nullptr && border->object() != nullptr &&
                boolean(border->field("inside"), true)) {
                const double width = non_negative_number(border->field("width"));
                style.scroll_viewport_insets = Edges{width, width, width, width};
            } else {
                style.scroll_viewport_insets = {};
            }
            style.scroll_viewport_insets_from_inside_border = false;
        }
        style.scroll_content_padding = edges(field(layout, "contentPadding"));
        style.scrollbar_gutter = non_negative_number(field(layout, "scrollbarGutter"));
    }
    const runtime::Value* scroll = field(layout, "scrollOffset");
    if (scroll == nullptr) scroll = scalar_property(description, "scrollOffset");
    if (scroll != nullptr && scroll->number() != nullptr) style.scroll_offset.y = *scroll->number();
    else if (scroll != nullptr && scroll->object() != nullptr) {
        style.scroll_offset = Point{number(scroll->field("x")), number(scroll->field("y"))};
    }
    const runtime::Value* pin = field(layout, "scrollPin");
    if (pin == nullptr) pin = scalar_property(description, "scrollPin");
    if (pin != nullptr) {
        style.pin_horizontal = boolean(pin->field("horizontal"));
        style.pin_vertical = boolean(pin->field("vertical"));
    }
    const double item_extent = number(field(layout, "virtualItemExtent"));
    const double item_count = number(field(layout, "virtualItemCount"));
    if (std::isfinite(item_extent) && item_extent > 0.0 && std::isfinite(item_count) && item_count >= 0.0) {
        VirtualListSpec spec;
        spec.axis = text(field(layout, "virtualAxis")) == "HORIZONTAL" ? LayoutAxis::horizontal : LayoutAxis::vertical;
        const std::size_t resolved_item_count = static_cast<std::size_t>(std::min(
            std::floor(item_count),
            static_cast<double>(std::numeric_limits<std::size_t>::max())
        ));
        spec.item_extent = item_extent;
        const double overscan = non_negative_number(field(layout, "virtualOverscan"), 1.0);
        spec.overscan = static_cast<std::size_t>(std::min(
            std::floor(overscan),
            static_cast<double>(std::numeric_limits<std::size_t>::max())
        ));
        if (const runtime::Value* keys = field(layout, "virtualItemKeys");
            keys != nullptr && keys->list() != nullptr &&
            keys->list()->values.size() == resolved_item_count) {
            std::vector<std::string> resolved;
            resolved.reserve(resolved_item_count);
            for (const runtime::Value& entry : keys->list()->values) {
                if (entry.key() != nullptr && !entry.key()->value.empty()) {
                    resolved.push_back(entry.key()->value);
                } else if (entry.string() != nullptr && !entry.string()->empty()) {
                    resolved.push_back(*entry.string());
                } else {
                    resolved.clear();
                    break;
                }
            }
            if (resolved.size() == resolved_item_count) {
                spec.items = std::make_shared<const SnapshotKeyedSequence>(std::move(resolved));
            }
        }
        if (description.virtual_sequence != nullptr &&
            description.virtual_sequence->count() == resolved_item_count) {
            spec.items = description.virtual_sequence;
        }
        if (spec.items == nullptr) {
            spec.items = std::make_shared<const IndexKeyedSequence>(resolved_item_count);
        }
        if (description.virtual_item_members != nullptr &&
            description.virtual_item_members->size() == resolved_item_count) {
            spec.item_members = description.virtual_item_members;
        } else if (const runtime::Value* members = field(layout, "virtualItemMembers");
                   members != nullptr && members->list() != nullptr &&
                   members->list()->values.size() == resolved_item_count) {
            VirtualItemMembers resolved;
            resolved.reserve(resolved_item_count);
            bool valid = true;
            for (const runtime::Value& band : members->list()->values) {
                if (band.list() == nullptr) {
                    valid = false;
                    break;
                }
                std::vector<std::string> item_keys;
                item_keys.reserve(band.list()->values.size());
                for (const runtime::Value& entry : band.list()->values) {
                    if (entry.key() != nullptr && !entry.key()->value.empty()) {
                        item_keys.push_back(entry.key()->value);
                    } else if (entry.string() != nullptr && !entry.string()->empty()) {
                        item_keys.push_back(*entry.string());
                    } else {
                        valid = false;
                        break;
                    }
                }
                if (!valid) break;
                resolved.push_back(std::move(item_keys));
            }
            if (valid) {
                spec.item_members = std::make_shared<const VirtualItemMembers>(
                    std::move(resolved)
                );
            }
        }
        if (description.virtual_item_extents != nullptr &&
            description.virtual_item_extents->size() == resolved_item_count) {
            spec.item_extents = *description.virtual_item_extents;
        } else if (const runtime::Value* extents = field(layout, "virtualItemExtents");
            extents != nullptr && extents->list() != nullptr &&
            extents->list()->values.size() == resolved_item_count) {
            std::vector<double> resolved;
            resolved.reserve(resolved_item_count);
            for (const runtime::Value& entry : extents->list()->values) {
                if (entry.number() == nullptr || !std::isfinite(*entry.number()) ||
                    *entry.number() <= 0.0) {
                    resolved.clear();
                    break;
                }
                resolved.push_back(*entry.number());
            }
            if (resolved.size() == resolved_item_count) {
                spec.item_extents.emplace(std::move(resolved));
            }
        }
        spec.measure_item_extents = boolean(field(layout, "virtualMeasureItemExtents"));
        spec.reset_anchor_on_change =
            text(field(layout, "virtualAnchorPolicy")) == "RESET_ON_CHANGE";
        style.virtual_list = spec;
    }
    if (const runtime::Value* target = field(layout, "portalTarget"); target != nullptr && target->string() != nullptr) {
        style.portal_target = *target->string();
    }
    style.detach_from_parent_clip = boolean(field(layout, "detachFromParentClip"), true);
    if (const runtime::Value* target = field(layout, "anchorTarget");
        target != nullptr && target->string() != nullptr) {
        style.anchor_target = *target->string();
    }
    if (const runtime::Value* point = field(layout, "anchorPoint");
        point != nullptr && point->object() != nullptr) {
        const double x = number(point->field("x"));
        const double y = number(point->field("y"));
        if (std::isfinite(x) && std::isfinite(y)) style.anchor_point = Point{x, y};
    }
    const std::string_view anchor_side = text(field(layout, "anchorSide"));
    if (anchor_side == "TOP") style.anchor_side = LayoutAnchorSide::top;
    else if (anchor_side == "RIGHT") style.anchor_side = LayoutAnchorSide::right;
    else if (anchor_side == "LEFT") style.anchor_side = LayoutAnchorSide::left;
    const std::string_view anchor_align = text(field(layout, "anchorAlign"));
    if (anchor_align == "CENTER") style.anchor_align = LayoutAnchorAlign::center;
    else if (anchor_align == "END") style.anchor_align = LayoutAnchorAlign::end;
    style.anchor_gap = non_negative_number(field(layout, "anchorGap"));
    style.anchor_flip = boolean(field(layout, "anchorFlip"), true);
    style.anchor_shift = boolean(field(layout, "anchorShift"), true);
    style.match_anchor_width = boolean(field(layout, "matchAnchorWidth"));
    return style;
}
} // namespace strata::ui
