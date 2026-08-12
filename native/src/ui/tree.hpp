#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/expression.hpp"
#include "runtime/sequence.hpp"
#include "runtime/value.hpp"
#include "ui/collection/virtualization.hpp"

namespace strata::ui {

struct DescriptionNode;
class Theme;

using VirtualItemMembers = std::vector<std::vector<std::string>>;

class DescriptionChildren {
  public:
    virtual ~DescriptionChildren() = default;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<const DescriptionNode> at(std::size_t index) const = 0;
};

class EagerDescriptionChildren final : public DescriptionChildren {
  public:
    explicit EagerDescriptionChildren(std::vector<std::shared_ptr<const DescriptionNode>> children);
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> at(std::size_t index) const override;

  private:
    std::vector<std::shared_ptr<const DescriptionNode>> children_;
};

class GeneratedDescriptionChildren final : public DescriptionChildren {
  public:
    using Factory = std::function<std::shared_ptr<const DescriptionNode>(std::size_t)>;

    GeneratedDescriptionChildren(std::size_t size, Factory factory);
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> at(std::size_t index) const override;

  private:
    std::size_t size_;
    Factory factory_;
    mutable std::mutex mutex_;
    /** Live rows are immutable; unreachable rows expire without an arbitrary numeric cache cap. */
    mutable std::map<std::size_t, std::weak_ptr<const DescriptionNode>> children_;
};

struct MaterializationRange final {
    std::size_t start = 0U;
    std::size_t end_exclusive = 0U;

    [[nodiscard]] friend bool operator==(const MaterializationRange&,
                                         const MaterializationRange&) = default;
};

/** One generated row build, published by Surface after that row is materialized. */
struct DescriptionMaterialization final {
    DescriptionMaterialization(runtime::StateScopeSet owned_state_scopes = {},
                               std::vector<runtime::RuntimeDiagnostic> diagnostics = {},
                               std::size_t evaluated_expressions = 0U,
                               std::size_t described_nodes = 0U);
    DescriptionMaterialization(const DescriptionMaterialization&) = delete;
    DescriptionMaterialization& operator=(const DescriptionMaterialization&) = delete;
    explicit DescriptionMaterialization(DescriptionMaterialization&& source) noexcept;
    DescriptionMaterialization& operator=(DescriptionMaterialization&&) = delete;

    /** Zero denotes only a moved-from shell; every publishable transaction owns one nonzero id. */
    [[nodiscard]] std::uint64_t transaction_id() const noexcept;

    runtime::StateScopeSet owned_state_scopes;
    std::vector<runtime::RuntimeDiagnostic> diagnostics;
    std::size_t evaluated_expressions = 0U;
    std::size_t described_nodes = 0U;

  private:
    std::uint64_t transaction_id_ = 0U;
};

/** Provider metadata which owns one retained virtual sequence generation. */
struct DescriptionSequenceGeneration final {
    std::uint64_t active_unit = 0U;
    runtime::ExpressionDependencyValue source;
    std::map<std::string, runtime::ExpressionDependencyValue, std::less<>> lexical_dependencies;
    std::map<std::string, runtime::ExpressionHostDependency, std::less<>> host_dependencies;

    [[nodiscard]] friend bool operator==(const DescriptionSequenceGeneration&,
                                         const DescriptionSequenceGeneration&) = default;
};

/**
 * A behavior attachment is executable description data, not a scalar property. Keeping the
 * action beside its scalar options avoids weakening runtime::Value with callable alternatives.
 */
struct DescriptionBehavior final {
    std::string id;
    bool enabled = true;
    runtime::Value options;
    std::shared_ptr<const runtime::ActionValue> action;
};

struct DescriptionNode final {
    using Properties = std::map<std::string, runtime::ExpressionValue, std::less<>>;

    std::string type;
    std::optional<std::string> key;
    std::string source_path;
    std::string state_scope;
    Properties properties;
    std::shared_ptr<const DescriptionChildren> children;
    std::optional<std::string> materialization_key;
    std::optional<MaterializationRange> materialization;
    /** Present only on a generated row root; copied through theme/materialization transforms. */
    std::shared_ptr<const DescriptionMaterialization> materialization_result;
    /** Keeps a weakly cached generated source alive exactly while a transformed row is retained. */
    std::shared_ptr<const DescriptionNode> generated_source;
    /** Effective inherited theme context for collection-local projection of entering virtual rows.
     */
    std::shared_ptr<const Theme> projected_theme;
    std::optional<std::string> projected_theme_scope;
    std::uint64_t projected_theme_generation = 0U;
    std::vector<DescriptionBehavior> behaviors;
    /** Data provider for virtual children; retained independently from row materialization. */
    std::shared_ptr<const runtime::IndexableSequence> virtual_sequence;
    std::optional<DescriptionSequenceGeneration> virtual_sequence_generation;
    /** Immutable domain metadata shared directly with layout and input without Value reparsing. */
    std::shared_ptr<const VirtualItemMembers> virtual_item_members{};
    std::shared_ptr<const collection::VirtualItemExtents> virtual_item_extents{};

    static std::shared_ptr<const DescriptionNode>
    create(std::string type, std::optional<std::string> key = std::nullopt,
           std::string source_path = {}, std::string state_scope = {}, Properties properties = {},
           std::shared_ptr<const DescriptionChildren> children = {},
           std::vector<DescriptionBehavior> behaviors = {});
};

enum class DirtyReason : std::uint32_t {
    structure = UINT32_C(1) << 0U,
    properties = UINT32_C(1) << 1U,
    layout = UINT32_C(1) << 2U,
    text = UINT32_C(1) << 3U,
    style = UINT32_C(1) << 4U,
    semantics = UINT32_C(1) << 5U,
    input = UINT32_C(1) << 6U,
    scale = UINT32_C(1) << 7U,
    animation = UINT32_C(1) << 8U,
    /** Editor-local selection/composition invalidation, intentionally outside semantic generations.
     */
    editor = UINT32_C(1) << 10U,
    /** Widget content changed without description, layout, text, or semantic work. */
    paint = UINT32_C(1) << 11U,
};

class DirtySet final {
  public:
    void add(DirtyReason reason) noexcept;
    void remove(DirtyReason reason) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool contains(DirtyReason reason) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::uint32_t bits() const noexcept;
    [[nodiscard]] friend bool operator==(const DirtySet&, const DirtySet&) = default;

  private:
    std::uint32_t bits_ = 0U;
};

struct DirtyGenerationSnapshot final {
    std::uint64_t structure = 0U;
    std::uint64_t properties = 0U;
    std::uint64_t layout = 0U;
    std::uint64_t text = 0U;
    std::uint64_t style = 0U;
    std::uint64_t semantics = 0U;
    std::uint64_t input = 0U;
    std::uint64_t scale = 0U;
    std::uint64_t animation = 0U;
    std::uint64_t editor = 0U;
    std::uint64_t paint = 0U;
    [[nodiscard]] friend bool operator==(const DirtyGenerationSnapshot&,
                                         const DirtyGenerationSnapshot&) = default;
};

enum class RetainedLifecycle { attached, exiting, detached };

/** Immutable retained-value view safe to keep in generated description factories. */
class RetainedDescriptionSnapshot final {
  public:
    struct Node final {
        std::uint64_t identity = 0U;
        std::string type;
        std::optional<std::string> key;
        std::string source_path;
        std::string state_scope;
        std::map<std::string, runtime::Value, std::less<>> retained_values;
        std::weak_ptr<const runtime::IndexableSequence> virtual_sequence;
        std::optional<DescriptionSequenceGeneration> virtual_sequence_generation;

        [[nodiscard]] const runtime::Value* retained_value(std::string_view name) const noexcept;
    };

    [[nodiscard]] const Node* find_key(std::string_view key) const noexcept;
    [[nodiscard]] const Node* find_key(std::string_view key, std::string_view source_path,
                                       std::string_view state_scope,
                                       std::string_view type) const noexcept;
    [[nodiscard]] const std::vector<const Node*>*
    find_source(std::string_view source_path) const noexcept;

  private:
    friend class RetainedTree;
    std::vector<std::unique_ptr<Node>> nodes_;
    std::map<std::string, std::vector<const Node*>, std::less<>> key_index_;
    std::map<std::string, std::vector<const Node*>, std::less<>> source_index_;
};

class RetainedNode final {
  public:
    using Cleanup = std::function<void()>;

    [[nodiscard]] std::uint64_t identity() const noexcept;
    [[nodiscard]] const DescriptionNode& description() const noexcept;
    [[nodiscard]] RetainedNode* parent() const noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<RetainedNode>>& children() const noexcept;
    [[nodiscard]] std::string_view structural_path() const noexcept;
    [[nodiscard]] std::size_t source_index() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t arrangement_revision() const noexcept;
    [[nodiscard]] std::uint64_t render_generation() const noexcept;
    /** Monotonic render identity for this node and every retained descendant. */
    [[nodiscard]] std::uint64_t subtree_render_generation() const noexcept;
    /** Overlay/foreground-only retained state, separate from widget-fragment invalidation. */
    [[nodiscard]] std::uint64_t presentation_generation() const noexcept;
    [[nodiscard]] std::uint64_t subtree_presentation_generation() const noexcept;
    [[nodiscard]] RetainedLifecycle lifecycle() const noexcept;
    [[nodiscard]] const DirtySet& dirty() const noexcept;
    /** Exact source range currently attached by this virtual collection's local realizer. */
    [[nodiscard]] std::optional<MaterializationRange> realized_range() const noexcept;
    /**
     * Returns an attached or warm themed row only when it belongs to the same immutable provider
     * and theme projection generation.
     */
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    realized_child_description(std::size_t source_index,
                               const std::shared_ptr<const DescriptionChildren>& provider,
                               const std::shared_ptr<const Theme>& projected_theme,
                               const std::optional<std::string>& projected_theme_scope,
                               std::uint64_t theme_generation) const noexcept;
    [[nodiscard]] bool
    realization_current(const std::shared_ptr<const DescriptionChildren>& provider,
                        const std::shared_ptr<const Theme>& projected_theme,
                        const std::optional<std::string>& projected_theme_scope,
                        std::uint64_t theme_generation, MaterializationRange range) const noexcept;
    /** State scopes kept alive by the collection's bounded warm description window. */
    [[nodiscard]] const runtime::StateScopeSet& warm_realization_state_scopes() const noexcept;
    [[nodiscard]] DirtyGenerationSnapshot dirty_generations() const noexcept;
    [[nodiscard]] const runtime::Value* retained_value(std::string_view name) const noexcept;
    [[nodiscard]] bool set_retained_value(std::string name, runtime::Value value);
    [[nodiscard]] std::span<const std::byte> retained_bytes(std::string_view name) const noexcept;
    [[nodiscard]] bool set_retained_bytes(std::string name, std::span<const std::byte> value);
    void add_cleanup(Cleanup cleanup);

  private:
    friend class RetainedTree;
    RetainedNode(std::uint64_t identity, std::shared_ptr<const DescriptionNode> description,
                 RetainedNode* parent, std::size_t source_index, std::string structural_path);
    void mark_dirty(DirtyReason reason);
    void refresh_subtree_metadata() noexcept;

    std::uint64_t identity_;
    std::shared_ptr<const DescriptionNode> description_;
    RetainedNode* parent_;
    std::vector<std::unique_ptr<RetainedNode>> children_;
    std::size_t source_index_;
    std::string structural_path_;
    std::uint64_t revision_ = 1U;
    std::uint64_t arrangement_revision_ = 1U;
    std::uint64_t render_generation_ = 0U;
    std::uint64_t subtree_render_generation_ = 0U;
    std::uint64_t presentation_generation_ = 0U;
    std::uint64_t subtree_presentation_generation_ = 0U;
    RetainedLifecycle lifecycle_ = RetainedLifecycle::attached;
    DirtySet dirty_;
    std::array<std::uint64_t, 11U> dirty_generations_{};
    std::map<std::string, runtime::Value, std::less<>> retained_values_;
    std::map<std::string, std::vector<std::byte>, std::less<>> retained_bytes_;
    std::vector<Cleanup> cleanups_;
    std::optional<MaterializationRange> realized_range_;
    std::shared_ptr<const DescriptionChildren> realization_provider_;
    std::shared_ptr<const Theme> realization_theme_;
    std::optional<std::string> realization_theme_scope_;
    std::uint64_t realization_theme_generation_ = 0U;
    std::map<std::size_t, std::shared_ptr<const DescriptionNode>> realization_cache_;
    std::deque<std::size_t> realization_cache_order_;
    runtime::StateScopeSet warm_realization_state_scopes_;
    std::size_t subtree_node_count_ = 1U;
    std::size_t subtree_materialized_child_count_ = 0U;
    bool subtree_all_attached_ = true;
};

struct ReconcileStats final {
    std::uint64_t generation = 0U;
    std::size_t created = 0U;
    std::size_t reused = 0U;
    std::size_t updated = 0U;
    std::size_t detached = 0U;
    std::size_t materialized = 0U;

    [[nodiscard]] bool changed() const noexcept;
};

struct RealizedDescriptionChild final {
    std::size_t source_index = 0U;
    std::shared_ptr<const DescriptionNode> description;
};

/** Stable retained identity tree with keyed reconciliation and eager detach cleanup. */
class RetainedTree final {
  public:
    using PersistenceFields = std::function<std::vector<std::string>(std::string_view type)>;
    using PersistenceReader = std::function<std::optional<runtime::Value>(
        std::string_view type, std::string_view key, std::string_view field)>;
    using PersistenceWriter = std::function<void(std::string_view key, std::string_view field,
                                                 const runtime::Value& value)>;
    using ExitRetention = std::function<bool(const RetainedNode&)>;
    using ExitCompletion = std::function<bool(const RetainedNode&)>;

    explicit RetainedTree(std::uint64_t identity_seed = 0U);
    ~RetainedTree();
    void configure_persistence(PersistenceFields fields, PersistenceReader reader,
                               PersistenceWriter writer);

    RetainedTree(const RetainedTree&) = delete;
    RetainedTree& operator=(const RetainedTree&) = delete;

    [[nodiscard]] ReconcileStats reconcile(std::shared_ptr<const DescriptionNode> root,
                                           const ExitRetention& exit_retention = {});
    /**
     * Locally reconciles one virtual collection's attached window without rebuilding its
     * declarative ancestors. Descriptions must be themed for the parent's projected context.
     */
    [[nodiscard]] ReconcileStats realize_children(
        std::uint64_t parent_identity, std::shared_ptr<const DescriptionChildren> provider,
        std::shared_ptr<const Theme> projected_theme,
        std::optional<std::string> projected_theme_scope, std::uint64_t theme_generation,
        MaterializationRange range, std::vector<RealizedDescriptionChild> children,
        const ExitRetention& exit_retention = {});
    /** Detaches completed EXITING child subtrees after the motion stage advances them. */
    [[nodiscard]] std::size_t prune_exiting(const ExitCompletion& exit_completion);
    [[nodiscard]] RetainedNode* root() const noexcept;
    [[nodiscard]] RetainedNode* find_key(std::string_view key) const noexcept;
    [[nodiscard]] const std::vector<RetainedNode*>* find_keys(std::string_view key) const noexcept;
    [[nodiscard]] RetainedNode* find_identity(std::uint64_t identity) const noexcept;
    [[nodiscard]] const std::vector<RetainedNode*>*
    find_source(std::string_view source_path) const noexcept;
    [[nodiscard]] const std::vector<RetainedNode*>* find_type(std::string_view type) const noexcept;
    [[nodiscard]] const std::vector<RetainedNode*>*
    find_state_scope(std::string_view scope) const noexcept;
    [[nodiscard]] const std::vector<RetainedNode*>& semantic_nodes() const noexcept;
    [[nodiscard]] const std::vector<RetainedNode*>& virtual_nodes() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::shared_ptr<const RetainedDescriptionSnapshot> description_snapshot() const;
    /** Monotonic epoch for changes that can affect measurement or arrangement. */
    [[nodiscard]] std::uint64_t layout_invalidation_generation() const noexcept;
    [[nodiscard]] std::uint64_t dirty_generation(DirtyReason reason) const noexcept;
    [[nodiscard]] DirtyGenerationSnapshot dirty_generations() const noexcept;
    [[nodiscard]] std::size_t dirty_count() const noexcept;
    /** Stable-identity scheduler input; contains only nodes with unconsumed dirty reasons. */
    [[nodiscard]] std::vector<RetainedNode*> dirty_nodes() const;
    [[nodiscard]] bool mark(std::uint64_t identity, DirtyReason reason);
    [[nodiscard]] bool set_retained_value(std::uint64_t identity, std::string name,
                                          runtime::Value value,
                                          DirtyReason reason = DirtyReason::properties);
    [[nodiscard]] bool set_retained_bytes(std::uint64_t identity, std::string name,
                                          std::span<const std::byte> value,
                                          DirtyReason reason = DirtyReason::properties);
    /** Updates interaction state consumed outside cached widget/layout/semantic projections. */
    [[nodiscard]] bool set_presentation_value(std::uint64_t identity, std::string name,
                                              runtime::Value value);
    /** Updates retained state consumed by the widget content paint callback only. */
    [[nodiscard]] bool set_paint_value(std::uint64_t identity, std::string name,
                                       runtime::Value value);
    /** Retained input/session state with no downstream frame work. */
    bool set_input_value(std::uint64_t identity, std::string name, runtime::Value value);
    bool set_input_bytes(std::uint64_t identity, std::string name,
                         std::span<const std::byte> value);
    /** Updates retained geometry state that requires arrangement but cannot affect measurement. */
    [[nodiscard]] bool set_arrangement_value(std::uint64_t identity, std::string name,
                                             runtime::Value value);
    void clear_dirty();
    /** Consumes the reasons owned by measurement/arrangement without touching other stages. */
    void consume_layout_dirty();
    void clear();

  private:
    [[nodiscard]] std::unique_ptr<RetainedNode>
    reconcile_node(std::unique_ptr<RetainedNode> existing,
                   std::shared_ptr<const DescriptionNode> description, RetainedNode* parent,
                   std::size_t source_index, std::string structural_path, ReconcileStats& stats,
                   bool& layout_invalidated, const ExitRetention& exit_retention);
    static void mark_subtree_lifecycle(RetainedNode& node, RetainedLifecycle lifecycle) noexcept;
    static void rebase_subtree(RetainedNode& node, RetainedNode* parent, std::size_t source_index,
                               std::string structural_path);
    void detach(std::unique_ptr<RetainedNode> node, ReconcileStats* stats) noexcept;
    void rebuild_indexes();
    void index(RetainedNode& node);
    void invalidate_description_snapshot() noexcept;
    [[nodiscard]] std::uint64_t next_identity();
    void invalidate_layout();
    void bump_dirty_generation(DirtyReason reason);
    void hydrate_persistence(RetainedNode& node);
    void persist_retained_value(const RetainedNode& node, std::string_view name,
                                const runtime::Value& value);
    [[nodiscard]] bool field_persisted(std::string_view type, std::string_view name) const;

    std::unique_ptr<RetainedNode> root_;
    std::array<std::uint64_t, 11U> dirty_generations_{};
    std::uint64_t generation_ = 0U;
    std::uint64_t layout_invalidation_generation_ = 0U;
    std::uint64_t next_identity_ = 1U;
    std::map<std::string, std::vector<RetainedNode*>, std::less<>> key_index_;
    std::map<std::uint64_t, RetainedNode*> identity_index_;
    std::map<std::string, std::vector<RetainedNode*>, std::less<>> source_index_;
    std::map<std::string, std::vector<RetainedNode*>, std::less<>> type_index_;
    std::map<std::string, std::vector<RetainedNode*>, std::less<>> state_scope_index_;
    std::vector<RetainedNode*> semantic_index_;
    std::vector<RetainedNode*> virtual_index_;
    std::set<std::uint64_t> dirty_index_;
    mutable std::shared_ptr<const RetainedDescriptionSnapshot> description_snapshot_;
    PersistenceFields persistence_fields_;
    PersistenceReader persistence_reader_;
    PersistenceWriter persistence_writer_;
};

/** State scopes owned by attached descriptions or an attached collection's bounded warm window. */
[[nodiscard]] runtime::StateScopeSet attached_description_state_scopes(const RetainedTree& tree);

[[nodiscard]] bool expression_value_equal(const runtime::ExpressionValue& left,
                                          const runtime::ExpressionValue& right);

} // namespace strata::ui
