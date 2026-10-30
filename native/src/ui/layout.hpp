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
#include <unordered_map>
#include <vector>

#include "geometry/primitives.hpp"
#include "runtime/diagnostic.hpp"
#include "ui/collection/virtualization.hpp"
#include "ui/motion/model.hpp"
#include "ui/tree.hpp"

namespace strata::ui {

class MotionRuntime;

using geometry::Edges;
using geometry::Point;
using geometry::Rect;
using geometry::Size;

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

/**
 * `masonry` packs children into equal columns, each into the column that is shortest so far;
 * `canvas` lays each child out at its own natural size, free of the box's size, at its inset.
 */
enum class LayoutKind {
    stack,
    row,
    column,
    grid,
    panel,
    overlay,
    spacer,
    scroll,
    portal,
    masonry,
    canvas
};
enum class LayoutAlign { start, center, end, stretch };
enum class LayoutJustify { start, center, end, space_between, space_around, space_evenly };
enum class LayoutAxis { horizontal, vertical };
enum class LayoutAnchorSide { bottom, top, right, left };
enum class LayoutAnchorAlign { start, center, end };
/**
 * How a child takes its place: in its parent's flow, out of flow against the parent's padding box
 * (`absolute`), or in flow but held inside the nearest scroll viewport by its insets (`sticky`).
 */
enum class LayoutPosition { flow, absolute, sticky };
/** Where a scroll container settles after the user scrolls: aligned to a child's edge or centre. */
enum class ScrollSnap { none, start, center, end };

/** Authored distances from the containing box's edges; an absent edge is unconstrained. */
struct LayoutInset final {
    std::optional<double> left;
    std::optional<double> top;
    std::optional<double> right;
    std::optional<double> bottom;

    [[nodiscard]] friend bool operator==(const LayoutInset&, const LayoutInset&) = default;
};

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
    LayoutPosition position = LayoutPosition::flow;
    LayoutInset inset;
    /** A ROW given less content width than this lays its children out as a COLUMN. */
    std::optional<double> stack_below;
    /** The node leaves layout while its parent's content width is below or above these. */
    std::optional<double> hide_below;
    std::optional<double> hide_above;
    bool wrap = false;
    bool clip = false;
    int z_index = 0;
    std::vector<LayoutSize> grid_columns;
    std::vector<LayoutSize> grid_rows;
    std::optional<std::size_t> grid_column;
    std::optional<std::size_t> grid_row;
    std::size_t column_span = 1U;
    std::size_t row_span = 1U;
    /** MASONRY: the column count, or the narrowest a column may be (columns follow the width). */
    std::optional<std::size_t> masonry_columns;
    std::optional<double> min_column_width;
    bool scroll_horizontal = false;
    bool scroll_vertical = true;
    ScrollSnap scroll_snap = ScrollSnap::none;
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
    /** It has a `clipShape`, which cuts what is hit on and within it. */
    bool clip_shaped = false;
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

/**
 * Masonry placement shared by measurement and arrangement: every child is as wide as a column and
 * sits at its offset from the content box's origin, in the column that was shortest when it came.
 */
struct MasonryLayoutResolution final {
    std::size_t columns = 1U;
    double column_width = 0.0;
    std::vector<Point> offsets;
    Size intrinsic_size;
    [[nodiscard]] friend bool operator==(const MasonryLayoutResolution&,
                                         const MasonryLayoutResolution&) = default;
};

/**
 * The column count a MASONRY box uses at `available` content width: `columns` when set (as a
 * maximum when `min_column_width` is also set), else as many columns of at least
 * `min_column_width` as fit, and always at least one.
 */
[[nodiscard]] std::size_t masonry_column_count(const LayoutStyle& style, double available) noexcept;

/**
 * Places children of the given heights into `columns` columns `column_width` wide: each into the
 * shortest column so far (the leftmost of equals), `gap` apart on both axes.
 */
[[nodiscard]] MasonryLayoutResolution resolve_masonry_layout(std::size_t columns,
                                                             double column_width,
                                                             const std::vector<double>& heights,
                                                             Point gap);

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
    /**
     * The virtual list does not scroll along its axis, so its window is what the viewports
     * around it show (see virtual_list_windowed_by_ancestors) rather than its own viewport.
     */
    bool virtual_windowed_by_ancestors = false;
    bool scroll_horizontal = false;
    bool scroll_vertical = false;
    bool pin_horizontal = false;
    bool pin_vertical = false;
    int z_index = 0;
    /** Absolute and sticky boxes paint and hit above in-flow siblings with the same zIndex. */
    bool positioned = false;
    std::vector<std::uint64_t> arranged_child_order;
    std::vector<std::size_t> materialized_child_indices;
    std::string portal_target;
    bool detached_from_parent_clip = false;
    /** Its node has a `clipShape` (LayoutStyle::clip_shaped). */
    bool clip_shaped = false;
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
    std::unordered_map<std::uint64_t, LayoutRecord> records;
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
    void retain(const std::unordered_map<std::uint64_t, LayoutRecord>& records);
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
    /**
     * A node's layout style as parsed from its description, cached on the node until its
     * description changes. Readers outside layout share it rather than parsing it each time.
     */
    [[nodiscard]] static const LayoutStyle& node_style(const RetainedNode& node);
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
        bool subtree_sticky = false;
        bool subtree_portals = false;
        /**
         * The subtree holds a virtual list windowed by the viewports around it, so moving it
         * relative to its clip changes what it realizes and it is never moved as a whole.
         */
        bool subtree_windowed = false;
        Size content_motion_target_size;
        std::vector<MeasuredNodePtr> children;
        /**
         * Prefix of children participating in parent flow; anchored, absolute and portal children
         * follow this prefix.
         */
        std::size_t flow_child_count = 0U;
        std::optional<LinearLayoutResolution> linear;
        std::optional<GridLayoutResolution> grid;
        std::optional<MasonryLayoutResolution> masonry;
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
        /** The nearest scroll viewport, which sticky descendants are held inside. */
        std::optional<Rect> sticky_viewport;
        /** The arranging parent's content box, which a sticky child never leaves. */
        std::optional<Rect> sticky_container;
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
    /**
     * The item extents of the virtual list `measured` lays out, learning the extents of the
     * items it measured: the estimate, an authored table, or what each item measured.
     */
    [[nodiscard]] collection::VirtualExtentResolution
    resolve_virtual_extents(const MeasuredNode& measured, double requested_offset);
    [[nodiscard]] LayoutStyle resolved_style(const RetainedNode& node) const;
    /** The node's style as parsed from its description, before runtime adjustments. */
    [[nodiscard]] static const ParsedLayout& parsed_layout(const RetainedNode& node);
    /**
     * The node's resolved style without a copy when nothing adjusts it at runtime (a scroll
     * offset, a gesture size, a running motion); otherwise resolved into adjusted.
     */
    [[nodiscard]] const LayoutStyle& style_of(const RetainedNode& node,
                                              std::optional<LayoutStyle>& adjusted) const;
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
    /**
     * What the records and caches held after they were last pruned of removed nodes: while the
     * tree's structure and their sizes are as they were, nothing in them is left to prune.
     */
    struct PrunedState final {
        const RetainedTree* tree = nullptr;
        std::uint64_t structure_generation = 0U;
        std::size_t records = 0U;
        std::size_t measurements = 0U;
        std::size_t arrangements = 0U;
        [[nodiscard]] friend bool operator==(const PrunedState&, const PrunedState&) = default;
    };
    std::optional<PrunedState> pruned_;
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
/**
 * Whether `style` is a virtual list that does not scroll along its axis. Such a list is as long as
 * all its items, sized to them, and the scroll viewports around it decide which items it
 * realizes: a long list sized to its content inside a scrolling page lays out only what the page
 * shows.
 */
[[nodiscard]] bool virtual_list_windowed_by_ancestors(const LayoutStyle& style) noexcept;
[[nodiscard]] std::optional<ContentSizeMotionSpec>
content_size_motion(const DescriptionNode& description);

/** What layout reads from a description, parsed once per description change. */
struct ParsedLayout final {
    LayoutStyle style;
    std::optional<ContentSizeMotionSpec> content_motion;
};
[[nodiscard]] Rect snap_rectangle(Rect rectangle, const LayoutEnvironment& environment);
[[nodiscard]] Point snap_point(Point point, const LayoutEnvironment& environment);
[[nodiscard]] std::string_view layout_kind_name(LayoutKind kind) noexcept;

} // namespace strata::ui
