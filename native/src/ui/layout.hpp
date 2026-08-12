#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "runtime/diagnostic.hpp"
#include "ui/collection/virtualization.hpp"
#include "ui/motion/model.hpp"
#include "ui/tree.hpp"

namespace strata::ui {

class MotionRuntime;

struct Point final {
    double x = 0.0;
    double y = 0.0;
    [[nodiscard]] friend bool operator==(const Point&, const Point&) = default;
};

struct Size final {
    double width = 0.0;
    double height = 0.0;
    [[nodiscard]] friend bool operator==(const Size&, const Size&) = default;
};

struct Edges final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] double horizontal() const noexcept;
    [[nodiscard]] double vertical() const noexcept;
    [[nodiscard]] friend bool operator==(const Edges&, const Edges&) = default;
};

struct Rect final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double right() const noexcept;
    [[nodiscard]] double bottom() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Rect deflate(const Edges& edges) const noexcept;
    /** Clip composition preserves an active zero-area rectangle instead of meaning "no clip". */
    [[nodiscard]] Rect clip_intersection(const Rect& other) const noexcept;
    [[nodiscard]] std::optional<Rect> intersection(const Rect& other) const noexcept;
    [[nodiscard]] friend bool operator==(const Rect&, const Rect&) = default;
};

struct Constraints final {
    double min_width = 0.0;
    double max_width = 0.0;
    double min_height = 0.0;
    double max_height = 0.0;

    [[nodiscard]] static Constraints unbounded() noexcept;
    [[nodiscard]] static Constraints fixed(double width, double height);
    [[nodiscard]] Constraints loosen() const noexcept;
    [[nodiscard]] Constraints deflate(const Edges& edges) const noexcept;
    [[nodiscard]] Size constrain(Size size) const noexcept;
    void validate() const;
    [[nodiscard]] friend bool operator==(const Constraints&, const Constraints&) = default;
};

enum class PointSnapPolicy { none, nearest };
enum class RectangleSnapPolicy { none, nearest, outward };

struct LayoutEnvironment final {
    std::uint64_t generation = 0U;
    Rect viewport;
    double scale = 1.0;
    Edges safe_insets;
    PointSnapPolicy point_snapping = PointSnapPolicy::nearest;
    RectangleSnapPolicy rectangle_snapping = RectangleSnapPolicy::outward;
    bool apply_safe_insets = true;
    std::int64_t frame_time_nanos = 0;
    bool reduced_motion = false;

    void validate() const;
    [[nodiscard]] friend bool operator==(const LayoutEnvironment&,
                                         const LayoutEnvironment&) = default;
};

enum class LayoutKind { stack, row, column, grid, panel, overlay, spacer, scroll, portal };
enum class LayoutAlign { start, center, end, stretch };
enum class LayoutJustify { start, center, end, space_between, space_around, space_evenly };
enum class LayoutAxis { horizontal, vertical };
enum class LayoutAnchorSide { bottom, top, right, left };
enum class LayoutAnchorAlign { start, center, end };

struct LayoutSize final {
    enum class Kind { automatic, content, fixed, percent, fill, clamp };

    Kind kind = Kind::automatic;
    double value = 0.0;
    std::shared_ptr<const LayoutSize> minimum;
    std::shared_ptr<const LayoutSize> preferred;
    std::shared_ptr<const LayoutSize> maximum;

    LayoutSize() = default;
    explicit LayoutSize(Kind kind, double value = 0.0) : kind(kind), value(value) {}
    [[nodiscard]] static LayoutSize clamp(std::optional<LayoutSize> minimum, LayoutSize preferred,
                                          std::optional<LayoutSize> maximum);
    friend bool operator==(const LayoutSize& left, const LayoutSize& right);
};

struct LayoutPlacement final {
    std::optional<LayoutSize> x;
    std::optional<LayoutSize> y;
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double offset_x = 0.0;
    double offset_y = 0.0;

    [[nodiscard]] friend bool operator==(const LayoutPlacement&, const LayoutPlacement&) = default;
};

struct VirtualListSpec final {
    LayoutAxis axis = LayoutAxis::vertical;
    std::shared_ptr<const runtime::KeyedSequence> items;
    double item_extent = 0.0;
    std::size_t overscan = 1U;
    /** Additional domain keys contained by one virtual item (for example an ItemGrid band). */
    std::shared_ptr<const VirtualItemMembers> item_members;
    std::optional<collection::VirtualItemExtents> item_extents;
    bool measure_item_extents = false;
    bool reset_anchor_on_change = false;

    [[nodiscard]] std::size_t item_count() const noexcept;
    [[nodiscard]] double total_item_extent() const noexcept;
    [[nodiscard]] double item_start(std::size_t index) const;
    [[nodiscard]] double extent_at(std::size_t index) const;
    [[nodiscard]] friend bool operator==(const VirtualListSpec&, const VirtualListSpec&) = default;
};

struct LayoutStyle final {
    bool participates = true;
    LayoutKind kind = LayoutKind::panel;
    LayoutSize width;
    LayoutSize height;
    std::optional<LayoutSize> min_width;
    std::optional<LayoutSize> min_height;
    std::optional<LayoutSize> max_width;
    std::optional<LayoutSize> max_height;
    std::optional<double> aspect_ratio;
    std::optional<Size> intrinsic_size;
    Edges padding;
    Edges margin;
    Point gap;
    LayoutAlign align_items = LayoutAlign::start;
    LayoutJustify justify_content = LayoutJustify::start;
    bool justify_content_authored = false;
    LayoutJustify align_content = LayoutJustify::start;
    std::optional<LayoutAlign> align_self;
    std::optional<LayoutAlign> justify_self;
    std::optional<LayoutPlacement> placement;
    bool wrap = false;
    bool clip = false;
    int z_index = 0;
    std::vector<LayoutSize> grid_columns;
    std::vector<LayoutSize> grid_rows;
    std::optional<std::size_t> grid_column;
    std::optional<std::size_t> grid_row;
    std::size_t column_span = 1U;
    std::size_t row_span = 1U;
    bool scroll_horizontal = false;
    bool scroll_vertical = true;
    Edges scroll_viewport_insets;
    bool scroll_viewport_insets_from_inside_border = false;
    Edges scroll_content_padding;
    double scrollbar_gutter = 0.0;
    Point scroll_offset;
    bool pin_horizontal = false;
    bool pin_vertical = false;
    std::optional<VirtualListSpec> virtual_list;
    std::string portal_target = "root";
    bool detach_from_parent_clip = true;
    std::string anchor_target;
    std::optional<Point> anchor_point;
    LayoutAnchorSide anchor_side = LayoutAnchorSide::bottom;
    LayoutAnchorAlign anchor_align = LayoutAnchorAlign::start;
    double anchor_gap = 0.0;
    double anchor_cross_offset = 0.0;
    bool anchor_flip = true;
    bool anchor_shift = true;
    bool match_anchor_width = false;

    [[nodiscard]] friend bool operator==(const LayoutStyle&, const LayoutStyle&) = default;
};

/** One resolved wrapped line. Indices refer to the parent's measured child sequence. */
struct LayoutLine final {
    std::vector<std::size_t> children;
    double main_size = 0.0;
    double cross_size = 0.0;
    [[nodiscard]] friend bool operator==(const LayoutLine&, const LayoutLine&) = default;
};

/** Shared measurement/arrangement result for a wrapping linear container. */
struct LinearLayoutResolution final {
    bool horizontal = true;
    std::vector<LayoutLine> lines;
    Size intrinsic_size;
    [[nodiscard]] friend bool operator==(const LinearLayoutResolution&,
                                         const LinearLayoutResolution&) = default;
};

/** A child-to-track assignment shared by grid measurement and arrangement. */
struct GridPlacement final {
    std::size_t child_index = 0U;
    std::size_t column = 0U;
    std::size_t row = 0U;
    std::size_t column_span = 1U;
    std::size_t row_span = 1U;
    [[nodiscard]] friend bool operator==(const GridPlacement&, const GridPlacement&) = default;
};

/** An intrinsic requirement crossing one or more tracks on a single grid axis. */
struct GridSpanContribution final {
    std::size_t start = 0U;
    std::size_t span = 1U;
    double extent = 0.0;
    [[nodiscard]] friend bool operator==(const GridSpanContribution&,
                                         const GridSpanContribution&) = default;
};

/** Track definitions and their intrinsic child contributions for one grid axis. */
struct GridAxisResolution final {
    std::vector<LayoutSize> tracks;
    std::vector<double> contributions;
    std::vector<GridSpanContribution> spanning_contributions;
    [[nodiscard]] friend bool operator==(const GridAxisResolution&,
                                         const GridAxisResolution&) = default;
};

/**
 * Grid topology and intrinsic contributions. Concrete track extents are resolved from this same
 * model against the content box in both measurement and arrangement.
 */
struct GridLayoutResolution final {
    GridAxisResolution columns;
    GridAxisResolution rows;
    std::vector<GridPlacement> placements;
    Size intrinsic_size;
    [[nodiscard]] friend bool operator==(const GridLayoutResolution&,
                                         const GridLayoutResolution&) = default;
};

struct VisibleRange final {
    std::size_t start = 0U;
    std::size_t end_exclusive = 0U;
    [[nodiscard]] friend bool operator==(const VisibleRange&, const VisibleRange&) = default;
};

struct LayoutRecord final {
    std::uint64_t identity = 0U;
    /** Pass in which this record was last written; exact cache hits retain the record in place. */
    std::uint64_t generation = 0U;
    /**
     * Changes only when this record's arranged presentation changes. The retained renderer uses
     * this narrower identity to update only genuinely changed nodes.
     */
    std::uint64_t render_generation = 0U;
    /** Changes when this record or any retained descendant changes arranged presentation. */
    std::uint64_t subtree_render_generation = 0U;
    /**
     * Set only for the pass in which the arrangement cache copied this record and its complete
     * retained subtree by one uniform translation. This is a fact produced by layout, not a
     * renderer inference from coincidentally similar rectangles.
     */
    std::optional<Point> translated_subtree;
    LayoutKind kind = LayoutKind::panel;
    Size measured_size;
    Rect bounds;
    Rect snapped_bounds;
    Rect hit_bounds;
    Rect content_bounds;
    std::optional<Rect> clip;
    std::optional<Rect> local_clip;
    std::optional<Rect> scroll_frame;
    std::optional<Rect> viewport;
    Size content_size;
    Point requested_scroll_offset;
    Point scroll_offset;
    std::optional<VisibleRange> visible_range;
    /** Key lookup for the complete virtual domain without a copied all-item key snapshot. */
    std::shared_ptr<const runtime::KeyedSequence> virtual_items;
    /** Keys in the currently observed/materialized window only. */
    std::vector<std::string> virtual_item_keys;
    std::size_t virtual_item_key_start = 0U;
    std::shared_ptr<const VirtualItemMembers> virtual_item_members;
    std::optional<collection::VirtualItemExtents> virtual_item_extents;
    std::optional<LayoutAxis> virtual_axis;
    std::size_t virtual_overscan = 0U;
    bool scroll_horizontal = false;
    bool scroll_vertical = false;
    bool pin_horizontal = false;
    bool pin_vertical = false;
    int z_index = 0;
    std::vector<std::uint64_t> arranged_child_order;
    std::vector<std::size_t> materialized_child_indices;
    std::string portal_target;
    bool detached_from_parent_clip = false;
    double content_motion_progress = 1.0;
    bool content_motion_running = false;
    bool content_motion_clip = false;
    bool content_motion_snapped_by_reduced_motion = false;
    Size content_motion_target_size;
};

struct LayoutOperationCounters final {
    std::size_t measured_nodes = 0U;
    std::size_t arranged_nodes = 0U;
    std::size_t translated_nodes = 0U;
    std::size_t measurement_cache_hits = 0U;
};

struct LayoutResult final {
    std::uint64_t generation = 0U;
    std::uint64_t root_identity = 0U;
    std::map<std::uint64_t, LayoutRecord> records;
    double scale = 1.0;
    LayoutOperationCounters operations;
    std::int64_t measure_nanos = 0;
    std::int64_t arrange_nanos = 0;
    std::int64_t maintenance_nanos = 0;

    [[nodiscard]] const LayoutRecord* find(std::uint64_t identity) const noexcept;
};

struct ContentSizeMotionSpec final {
    bool animate_width = false;
    bool animate_height = true;
    bool clip = true;
    MotionTiming timing{
        180'000'000, 0, "cubic-in-out", {}, false, MotionFillMode::both,
    };
    [[nodiscard]] friend bool operator==(const ContentSizeMotionSpec&,
                                         const ContentSizeMotionSpec&) = default;
};

struct ContentSizeMotionSample final {
    Size size;
    Size target;
    double progress = 1.0;
    bool running = false;
    bool clip = false;
    bool snapped_by_reduced_motion = false;
};

/** Interruption-safe measured content-size transitions indexed only while active. */
class ContentSizeTransitions final {
  public:
    [[nodiscard]] ContentSizeMotionSample retarget(std::uint64_t identity, Size target,
                                                   std::int64_t now_nanos,
                                                   const ContentSizeMotionSpec& spec,
                                                   bool reduced_motion);
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] std::vector<std::uint64_t> active_identities() const;
    void remove(std::uint64_t identity) noexcept;
    void retain(const std::map<std::uint64_t, LayoutRecord>& records);
    void clear() noexcept;

  private:
    struct Active final {
        Size from;
        Size target;
        std::int64_t started_nanos;
        ContentSizeMotionSpec spec;
    };

    [[nodiscard]] ContentSizeMotionSample sample(std::uint64_t identity, std::int64_t now_nanos);

    std::map<std::uint64_t, Size> targets_;
    std::map<std::uint64_t, Active> active_;
};

/** Retained, backend-independent measurement and arrangement engine. */
class LayoutEngine final {
  public:
    using IntrinsicMeasure = std::function<Size(const RetainedNode&, const Constraints&)>;

    explicit LayoutEngine(IntrinsicMeasure intrinsic_measure = {});
    [[nodiscard]] const LayoutResult& layout(RetainedTree& tree,
                                             const LayoutEnvironment& environment,
                                             const MotionRuntime* motion = nullptr,
                                             bool consume_dirty = true,
                                             std::uint64_t frame_index = 0U);
    [[nodiscard]] const LayoutResult& result() const noexcept;
    [[nodiscard]] std::size_t active_transition_count() const noexcept;
    [[nodiscard]] bool requires_layout(const RetainedTree& tree,
                                       const LayoutEnvironment& environment) const;
    [[nodiscard]] std::vector<runtime::RuntimeDiagnostic> take_diagnostics();
    void clear_diagnostics() noexcept;
    void clear();

  private:
    struct MeasuredNode;
    using MeasuredNodePtr = std::shared_ptr<const MeasuredNode>;

    struct MeasuredNode final {
        const RetainedNode* node = nullptr;
        LayoutStyle style;
        Size measured_size;
        Size content_size;
        bool content_motion_clip = false;
        double content_motion_progress = 1.0;
        bool content_motion_running = false;
        bool content_motion_snapped_by_reduced_motion = false;
        bool subtree_pins_horizontal = false;
        bool subtree_pins_vertical = false;
        bool subtree_portals = false;
        Size content_motion_target_size;
        std::vector<MeasuredNodePtr> children;
        /** Prefix of children participating in parent flow; portals follow this prefix. */
        std::size_t flow_child_count = 0U;
        std::optional<LinearLayoutResolution> linear;
        std::optional<GridLayoutResolution> grid;
    };

    struct MeasurementCacheEntry final {
        Constraints constraints;
        std::uint64_t node_revision = 0U;
        std::uint64_t environment_generation = 0U;
        double scale = 1.0;
        MeasuredNodePtr measured;
        /** Stable identity propagated while this subtree's parent-facing contribution is equal. */
        MeasuredNodePtr propagated;
    };

    struct PinContext final {
        std::optional<double> horizontal_offset;
        std::optional<double> vertical_offset;
        [[nodiscard]] friend bool operator==(const PinContext&, const PinContext&) = default;
    };

    struct PendingPortal final {
        MeasuredNodePtr measured;
        std::optional<Rect> inherited_clip;
        PinContext pin_context;
    };

    struct PendingAnchor final {
        MeasuredNodePtr measured;
        Rect fallback_bounds;
        Rect containing_bounds;
        std::optional<Rect> inherited_clip;
        PinContext pin_context;
    };

    struct ArrangementCacheEntry final {
        std::weak_ptr<const MeasuredNode> measured;
        Rect bounds;
        std::optional<Rect> inherited_clip;
        PinContext pin_context;
        std::uint64_t node_arrangement_revision = 0U;
    };

    [[nodiscard]] MeasuredNodePtr measure(const RetainedNode& node, const Constraints& constraints,
                                          const LayoutEnvironment& environment,
                                          LayoutOperationCounters& operations, bool force = false);
    [[nodiscard]] static bool measured_model_equal(const MeasuredNode& left,
                                                   const MeasuredNode& right);
    void arrange(const MeasuredNodePtr& measured, Rect bounds, std::optional<Rect> inherited_clip,
                 PinContext pin_context, const LayoutEnvironment& environment,
                 LayoutResult& result);
    [[nodiscard]] LayoutStyle resolved_style(const RetainedNode& node) const;
    [[nodiscard]] static Point resolved_scroll_offset(const RetainedNode& node,
                                                      Point fallback) noexcept;
    [[nodiscard]] std::uint64_t advance_render_generation();
    [[nodiscard]] bool arranged_in_current_pass(const RetainedNode& node) const noexcept;

    IntrinsicMeasure intrinsic_measure_;
    std::map<std::uint64_t, MeasurementCacheEntry> measurement_cache_;
    std::map<std::uint64_t, MeasuredNodePtr> measurement_frontier_;
    /** Nodes already measured against their current constraints during the active layout pass. */
    std::unordered_set<std::uint64_t> measurement_pass_;
    /** Current-pass structural changes which must propagate fresh node pointers to the root. */
    std::unordered_set<std::uint64_t> structural_measurements_;
    std::map<std::uint64_t, ArrangementCacheEntry> arrangement_cache_;
    std::vector<PendingPortal> pending_portals_;
    std::vector<PendingAnchor> pending_anchors_;
    /**
     * Exact arrangement-cache hits retain records in place. These sets distinguish those current
     * records from stale records which have not participated in the active pass.
     */
    std::unordered_set<std::uint64_t> current_arranged_records_;
    std::unordered_set<std::uint64_t> current_arranged_subtree_roots_;
    /** Records carrying a translation accumulated across one Surface frame's convergence passes. */
    std::vector<std::uint64_t> translated_records_;
    std::uint64_t translation_frame_index_ = 0U;
    LayoutResult result_;
    const RetainedTree* last_tree_ = nullptr;
    std::uint64_t last_invalidation_generation_ = 0U;
    std::optional<LayoutEnvironment> last_environment_;
    std::uint64_t next_generation_ = 0U;
    std::uint64_t next_render_generation_ = 0U;
    ContentSizeTransitions content_size_transitions_;
    collection::VirtualizationCache collection_virtualization_;
    const MotionRuntime* motion_ = nullptr;
    std::vector<runtime::RuntimeDiagnostic> diagnostics_;
    std::set<std::string, std::less<>> reported_diagnostics_;
};

[[nodiscard]] LayoutStyle layout_style(const DescriptionNode& description);
[[nodiscard]] std::optional<ContentSizeMotionSpec>
content_size_motion(const DescriptionNode& description);
[[nodiscard]] Rect snap_rectangle(Rect rectangle, const LayoutEnvironment& environment);
[[nodiscard]] Point snap_point(Point point, const LayoutEnvironment& environment);
[[nodiscard]] std::string_view layout_kind_name(LayoutKind kind) noexcept;

} // namespace strata::ui
