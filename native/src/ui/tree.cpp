#include "ui/tree.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] std::uint64_t next_materialization_transaction_id() {
    static std::atomic<std::uint64_t> next{1U};
    std::uint64_t current = next.load(std::memory_order_relaxed);
    for (;;) {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("description materialization transaction id exhausted");
        }
        if (next.compare_exchange_weak(
                current,
                current + 1U,
                std::memory_order_relaxed,
                std::memory_order_relaxed
            )) {
            return current;
        }
    }
}

void validate_text(const std::string_view value, const std::string_view label, const bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || !core::valid_utf8(value)) {
        throw std::invalid_argument(std::string(label) + " must be valid UTF-8" + (allow_empty ? "" : " and non-empty"));
    }
}

[[nodiscard]] bool affects_layout(const DirtyReason reason) noexcept {
    return reason == DirtyReason::structure || reason == DirtyReason::layout ||
           reason == DirtyReason::text || reason == DirtyReason::style ||
           reason == DirtyReason::scale || reason == DirtyReason::resource ||
           reason == DirtyReason::editor;
}

[[nodiscard]] bool invalidates_render(const DirtyReason reason) noexcept {
    return reason != DirtyReason::semantics;
}

[[nodiscard]] bool action_value_equal(
    const std::shared_ptr<const runtime::ActionValue>& left,
    const std::shared_ptr<const runtime::ActionValue>& right
) {
    if (left == right) return true;
    if (left == nullptr || right == nullptr || left->composition != right->composition ||
        left->children.size() != right->children.size()) {
        return false;
    }
    if ((left->action == nullptr) != (right->action == nullptr)) return false;
    if (left->action != nullptr &&
        (left->action->id() != right->action->id() ||
         left->action->payload != right->action->payload ||
         left->action->dynamic != right->action->dynamic)) {
        return false;
    }
    for (std::size_t index = 0U; index < left->children.size(); ++index) {
        if (!action_value_equal(left->children[index], right->children[index])) return false;
    }
    return true;
}

[[nodiscard]] std::size_t reason_index(const DirtyReason reason) noexcept {
    switch (reason) {
    case DirtyReason::structure: return 0U;
    case DirtyReason::properties: return 1U;
    case DirtyReason::layout: return 2U;
    case DirtyReason::text: return 3U;
    case DirtyReason::style: return 4U;
    case DirtyReason::semantics: return 5U;
    case DirtyReason::input: return 6U;
    case DirtyReason::scale: return 7U;
    case DirtyReason::animation: return 8U;
    case DirtyReason::resource: return 9U;
    case DirtyReason::editor: return 10U;
    }
    return 0U;
}

} // namespace

DescriptionMaterialization::DescriptionMaterialization(
    runtime::StateScopeSet source_owned_state_scopes,
    std::vector<runtime::RuntimeDiagnostic> source_diagnostics,
    const std::size_t source_evaluated_expressions,
    const std::size_t source_described_nodes
) : owned_state_scopes(std::move(source_owned_state_scopes)),
    diagnostics(std::move(source_diagnostics)),
    evaluated_expressions(source_evaluated_expressions),
    described_nodes(source_described_nodes),
    transaction_id_(next_materialization_transaction_id()) {}

DescriptionMaterialization::DescriptionMaterialization(
    DescriptionMaterialization&& source
) noexcept : owned_state_scopes(std::move(source.owned_state_scopes)),
    diagnostics(std::move(source.diagnostics)),
    evaluated_expressions(std::exchange(source.evaluated_expressions, 0U)),
    described_nodes(std::exchange(source.described_nodes, 0U)),
    transaction_id_(std::exchange(source.transaction_id_, 0U)) {}

std::uint64_t DescriptionMaterialization::transaction_id() const noexcept {
    return transaction_id_;
}

EagerDescriptionChildren::EagerDescriptionChildren(
    std::vector<std::shared_ptr<const DescriptionNode>> children
) : children_(std::move(children)) {
    if (std::ranges::any_of(children_, [](const auto& child) { return child == nullptr; })) {
        throw std::invalid_argument("eager description children must not contain null nodes");
    }
}

std::size_t EagerDescriptionChildren::size() const noexcept { return children_.size(); }

std::shared_ptr<const DescriptionNode> EagerDescriptionChildren::at(const std::size_t index) const {
    if (index >= children_.size()) throw std::out_of_range("description child index is outside the eager snapshot");
    return children_[index];
}

GeneratedDescriptionChildren::GeneratedDescriptionChildren(
    const std::size_t size,
    Factory factory
) : size_(size), factory_(std::move(factory)) {
    if (!factory_) throw std::invalid_argument("generated description children require a factory");
}

std::size_t GeneratedDescriptionChildren::size() const noexcept { return size_; }

std::shared_ptr<const DescriptionNode> GeneratedDescriptionChildren::at(const std::size_t index) const {
    if (index >= size_) throw std::out_of_range("description child index is outside the generated snapshot");
    std::scoped_lock lock(mutex_);
    std::erase_if(children_, [](const auto& entry) { return entry.second.expired(); });
    if (const auto found = children_.find(index); found != children_.end()) {
        if (std::shared_ptr<const DescriptionNode> child = found->second.lock()) return child;
        children_.erase(found);
    }
    std::shared_ptr<const DescriptionNode> child = factory_(index);
    if (child == nullptr) throw std::logic_error("generated description child factory returned null");
    children_.emplace(index, child);
    return child;
}

std::shared_ptr<const DescriptionNode> DescriptionNode::create(
    std::string type,
    std::optional<std::string> key,
    std::string source_path,
    std::string state_scope,
    Properties properties,
    std::shared_ptr<const DescriptionChildren> children,
    std::vector<DescriptionBehavior> behaviors
) {
    validate_text(type, "description type");
    if (key.has_value()) validate_text(*key, "description key");
    validate_text(source_path, "description source path", true);
    validate_text(state_scope, "description state scope", true);
    for (const auto& [name, value] : properties) {
        static_cast<void>(value);
        validate_text(name, "description property");
    }
    std::set<std::string, std::less<>> behavior_ids;
    for (const DescriptionBehavior& behavior : behaviors) {
        validate_text(behavior.id, "description behavior id");
        if (!behavior_ids.emplace(behavior.id).second) {
            throw std::invalid_argument("a description node cannot attach the same behavior twice");
        }
    }
    if (children == nullptr) {
        children = std::make_shared<const EagerDescriptionChildren>(
            std::vector<std::shared_ptr<const DescriptionNode>>{}
        );
    }
    return std::make_shared<const DescriptionNode>(DescriptionNode{
        std::move(type),
        std::move(key),
        std::move(source_path),
        std::move(state_scope),
        std::move(properties),
        std::move(children),
        std::nullopt,
        std::nullopt,
        {},
        {},
        {},
        std::nullopt,
        0U,
        std::move(behaviors),
        {},
        std::nullopt,
    });
}

const runtime::Value* RetainedDescriptionSnapshot::Node::retained_value(
    const std::string_view name
) const noexcept {
    const auto found = retained_values.find(name);
    return found != retained_values.end() ? &found->second : nullptr;
}

const RetainedDescriptionSnapshot::Node* RetainedDescriptionSnapshot::find_key(
    const std::string_view key
) const noexcept {
    const auto found = key_index_.find(key);
    return found != key_index_.end() ? found->second : nullptr;
}

const std::vector<const RetainedDescriptionSnapshot::Node*>*
RetainedDescriptionSnapshot::find_source(const std::string_view source_path) const noexcept {
    const auto found = source_index_.find(source_path);
    return found != source_index_.end() ? &found->second : nullptr;
}

void DirtySet::add(const DirtyReason reason) noexcept { bits_ |= static_cast<std::uint32_t>(reason); }
void DirtySet::remove(const DirtyReason reason) noexcept { bits_ &= ~static_cast<std::uint32_t>(reason); }
void DirtySet::clear() noexcept { bits_ = 0U; }
bool DirtySet::contains(const DirtyReason reason) const noexcept {
    return (bits_ & static_cast<std::uint32_t>(reason)) != 0U;
}
bool DirtySet::empty() const noexcept { return bits_ == 0U; }
std::uint32_t DirtySet::bits() const noexcept { return bits_; }

RetainedNode::RetainedNode(
    const std::uint64_t identity,
    std::shared_ptr<const DescriptionNode> description,
    RetainedNode* const parent,
    const std::size_t source_index,
    std::string structural_path
) : identity_(identity),
    description_(std::move(description)),
    parent_(parent),
    source_index_(source_index),
    structural_path_(std::move(structural_path)) {
    mark_dirty(DirtyReason::structure);
    mark_dirty(DirtyReason::properties);
    mark_dirty(DirtyReason::layout);
}

void RetainedNode::mark_dirty(const DirtyReason reason) {
    std::uint64_t& generation = dirty_generations_[reason_index(reason)];
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("retained node dirty generation exhausted");
    }
    ++generation;
    dirty_.add(reason);
    if (invalidates_render(reason)) {
        if (render_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("retained node render generation exhausted");
        }
        ++render_generation_;
        for (RetainedNode* ancestor = this; ancestor != nullptr; ancestor = ancestor->parent_) {
            if (ancestor->subtree_render_generation_ ==
                std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error(
                    "retained subtree render generation exhausted"
                );
            }
            ++ancestor->subtree_render_generation_;
        }
    }
}

std::uint64_t RetainedNode::identity() const noexcept { return identity_; }
const DescriptionNode& RetainedNode::description() const noexcept { return *description_; }
RetainedNode* RetainedNode::parent() const noexcept { return parent_; }
const std::vector<std::unique_ptr<RetainedNode>>& RetainedNode::children() const noexcept { return children_; }
std::string_view RetainedNode::structural_path() const noexcept { return structural_path_; }
std::size_t RetainedNode::source_index() const noexcept { return source_index_; }
std::uint64_t RetainedNode::revision() const noexcept { return revision_; }
std::uint64_t RetainedNode::arrangement_revision() const noexcept {
    return arrangement_revision_;
}
std::uint64_t RetainedNode::render_generation() const noexcept { return render_generation_; }
std::uint64_t RetainedNode::subtree_render_generation() const noexcept {
    return subtree_render_generation_;
}
std::uint64_t RetainedNode::presentation_generation() const noexcept {
    return presentation_generation_;
}
std::uint64_t RetainedNode::subtree_presentation_generation() const noexcept {
    return subtree_presentation_generation_;
}
RetainedLifecycle RetainedNode::lifecycle() const noexcept { return lifecycle_; }
const DirtySet& RetainedNode::dirty() const noexcept { return dirty_; }
std::optional<MaterializationRange> RetainedNode::realized_range() const noexcept {
    return realized_range_;
}

std::shared_ptr<const DescriptionNode> RetainedNode::realized_child_description(
    const std::size_t source_index,
    const std::shared_ptr<const DescriptionChildren>& provider,
    const std::shared_ptr<const Theme>& projected_theme,
    const std::optional<std::string>& projected_theme_scope,
    const std::uint64_t theme_generation
) const noexcept {
    if (provider == nullptr || realization_provider_ != provider ||
        realization_theme_ != projected_theme ||
        realization_theme_scope_ != projected_theme_scope ||
        realization_theme_generation_ != theme_generation) {
        return nullptr;
    }
    for (const auto& child : children_) {
        if (child->lifecycle_ == RetainedLifecycle::attached &&
            child->source_index_ == source_index) {
            return child->description_;
        }
    }
    const auto found = realization_cache_.find(source_index);
    return found != realization_cache_.end() ? found->second : nullptr;
}

bool RetainedNode::realization_current(
    const std::shared_ptr<const DescriptionChildren>& provider,
    const std::shared_ptr<const Theme>& projected_theme,
    const std::optional<std::string>& projected_theme_scope,
    const std::uint64_t theme_generation,
    const MaterializationRange range
) const noexcept {
    return realization_provider_ == provider &&
           realization_theme_ == projected_theme &&
           realization_theme_scope_ == projected_theme_scope &&
           realization_theme_generation_ == theme_generation &&
           realized_range_ == std::optional<MaterializationRange>(range);
}

const runtime::StateScopeSet& RetainedNode::warm_realization_state_scopes() const noexcept {
    return warm_realization_state_scopes_;
}

DirtyGenerationSnapshot RetainedNode::dirty_generations() const noexcept {
    return DirtyGenerationSnapshot{
        dirty_generations_[0U], dirty_generations_[1U], dirty_generations_[2U],
        dirty_generations_[3U], dirty_generations_[4U], dirty_generations_[5U],
        dirty_generations_[6U], dirty_generations_[7U], dirty_generations_[8U],
        dirty_generations_[9U], dirty_generations_[10U]
    };
}

const runtime::Value* RetainedNode::retained_value(const std::string_view name) const noexcept {
    const auto found = retained_values_.find(name);
    return found != retained_values_.end() ? &found->second : nullptr;
}

bool RetainedNode::set_retained_value(std::string name, runtime::Value value) {
    validate_text(name, "retained value name");
    const auto found = retained_values_.find(name);
    if (found != retained_values_.end() && found->second == value) return false;
    retained_values_.insert_or_assign(std::move(name), std::move(value));
    return true;
}

void RetainedNode::add_cleanup(Cleanup cleanup) {
    if (!cleanup) throw std::invalid_argument("retained cleanup callback must not be empty");
    cleanups_.push_back(std::move(cleanup));
}

void RetainedNode::refresh_subtree_metadata() noexcept {
    subtree_node_count_ = 1U;
    subtree_materialized_child_count_ = children_.size();
    subtree_all_attached_ = lifecycle_ == RetainedLifecycle::attached;
    for (const auto& child : children_) {
        subtree_node_count_ += child->subtree_node_count_;
        subtree_materialized_child_count_ += child->subtree_materialized_child_count_;
        subtree_all_attached_ = subtree_all_attached_ && child->subtree_all_attached_;
    }
}

bool ReconcileStats::changed() const noexcept {
    return created != 0U || updated != 0U || detached != 0U;
}

RetainedTree::RetainedTree(const std::uint64_t identity_seed) : next_identity_(identity_seed) {}
RetainedTree::~RetainedTree() { clear(); }

void RetainedTree::configure_persistence(
    PersistenceFields fields,
    PersistenceReader reader,
    PersistenceWriter writer
) {
    persistence_fields_ = std::move(fields);
    persistence_reader_ = std::move(reader);
    persistence_writer_ = std::move(writer);
}

RetainedNode* RetainedTree::root() const noexcept { return root_.get(); }

std::shared_ptr<const RetainedDescriptionSnapshot> RetainedTree::description_snapshot() const {
    if (description_snapshot_ != nullptr) return description_snapshot_;
    auto snapshot = std::make_shared<RetainedDescriptionSnapshot>();
    if (root_ == nullptr) {
        description_snapshot_ = snapshot;
        return snapshot;
    }
    const auto append = [&snapshot](const auto& self, const RetainedNode& retained) -> void {
        auto node = std::make_unique<RetainedDescriptionSnapshot::Node>();
        node->identity = retained.identity_;
        node->type = retained.description_->type;
        node->key = retained.description_->key;
        node->source_path = retained.description_->source_path;
        node->state_scope = retained.description_->state_scope;
        node->retained_values = retained.retained_values_;
        node->virtual_sequence = retained.description_->virtual_sequence;
        node->virtual_sequence_generation =
            retained.description_->virtual_sequence_generation;
        const RetainedDescriptionSnapshot::Node* const indexed = node.get();
        snapshot->nodes_.push_back(std::move(node));
        if (indexed->key.has_value()) {
            snapshot->key_index_.try_emplace(*indexed->key, indexed);
        }
        snapshot->source_index_[indexed->source_path].push_back(indexed);
        for (const std::unique_ptr<RetainedNode>& child : retained.children_) {
            self(self, *child);
        }
    };
    append(append, *root_);
    description_snapshot_ = snapshot;
    return description_snapshot_;
}

RetainedNode* RetainedTree::find_key(const std::string_view key) const noexcept {
    const auto found = key_index_.find(key);
    return found != key_index_.end() ? found->second : nullptr;
}

RetainedNode* RetainedTree::find_identity(const std::uint64_t identity) const noexcept {
    const auto found = identity_index_.find(identity);
    return found != identity_index_.end() ? found->second : nullptr;
}

const std::vector<RetainedNode*>* RetainedTree::find_source(const std::string_view source_path) const noexcept {
    const auto found = source_index_.find(source_path);
    return found != source_index_.end() ? &found->second : nullptr;
}

const std::vector<RetainedNode*>* RetainedTree::find_type(const std::string_view type) const noexcept {
    const auto found = type_index_.find(type);
    return found != type_index_.end() ? &found->second : nullptr;
}

const std::vector<RetainedNode*>* RetainedTree::find_state_scope(const std::string_view scope) const noexcept {
    const auto found = state_scope_index_.find(scope);
    return found != state_scope_index_.end() ? &found->second : nullptr;
}

const std::vector<RetainedNode*>& RetainedTree::semantic_nodes() const noexcept { return semantic_index_; }
const std::vector<RetainedNode*>& RetainedTree::virtual_nodes() const noexcept {
    return virtual_index_;
}

std::uint64_t RetainedTree::generation() const noexcept { return generation_; }
std::uint64_t RetainedTree::layout_invalidation_generation() const noexcept {
    return layout_invalidation_generation_;
}

std::uint64_t RetainedTree::dirty_generation(const DirtyReason reason) const noexcept {
    return dirty_generations_[reason_index(reason)];
}

DirtyGenerationSnapshot RetainedTree::dirty_generations() const noexcept {
    return DirtyGenerationSnapshot{
        dirty_generations_[0U],
        dirty_generations_[1U],
        dirty_generations_[2U],
        dirty_generations_[3U],
        dirty_generations_[4U],
        dirty_generations_[5U],
        dirty_generations_[6U],
        dirty_generations_[7U],
        dirty_generations_[8U],
        dirty_generations_[9U],
        dirty_generations_[10U],
    };
}
std::size_t RetainedTree::dirty_count() const noexcept { return dirty_index_.size(); }

std::vector<RetainedNode*> RetainedTree::dirty_nodes() const {
    std::vector<RetainedNode*> result;
    result.reserve(dirty_index_.size());
    for (const std::uint64_t identity : dirty_index_) {
        if (RetainedNode* node = find_identity(identity); node != nullptr) {
            result.push_back(node);
        }
    }
    return result;
}

runtime::StateScopeSet attached_description_state_scopes(const RetainedTree& tree) {
    runtime::StateScopeSet result;
    if (tree.root() == nullptr) return result;
    const auto collect = [&result](const auto& self, const RetainedNode& node) -> void {
        if (node.lifecycle() != RetainedLifecycle::attached) return;
        if (node.description().materialization_result != nullptr) {
            const auto& scopes = node.description().materialization_result->owned_state_scopes;
            result.insert(scopes.begin(), scopes.end());
        }
        const runtime::StateScopeSet& warm = node.warm_realization_state_scopes();
        result.insert(warm.begin(), warm.end());
        for (const std::unique_ptr<RetainedNode>& child : node.children()) {
            self(self, *child);
        }
    };
    collect(collect, *tree.root());
    return result;
}

bool RetainedTree::mark(const std::uint64_t identity, const DirtyReason reason) {
    RetainedNode* node = find_identity(identity);
    if (node == nullptr) return false;
    const bool was_dirty = node->dirty_.contains(reason);
    node->mark_dirty(reason);
    dirty_index_.insert(identity);
    bump_dirty_generation(reason);
    if (affects_layout(reason)) {
        for (RetainedNode* current = node; current != nullptr; current = current->parent_) {
            if (current->revision_ == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("retained node revision exhausted");
            }
            ++current->revision_;
        }
        invalidate_layout();
    }
    return !was_dirty;
}

bool RetainedTree::set_retained_value(
    const std::uint64_t identity,
    std::string name,
    runtime::Value value,
    const DirtyReason reason
) {
    RetainedNode* node = find_identity(identity);
    if (node == nullptr || !node->set_retained_value(name, value)) return false;
    persist_retained_value(*node, name, value);
    invalidate_description_snapshot();
    static_cast<void>(mark(identity, reason));
    return true;
}

bool RetainedTree::set_presentation_value(
    const std::uint64_t identity,
    std::string name,
    runtime::Value value
) {
    RetainedNode* node = find_identity(identity);
    if (node == nullptr || !node->set_retained_value(std::move(name), std::move(value))) {
        return false;
    }
    if (node->presentation_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("retained node presentation generation exhausted");
    }
    ++node->presentation_generation_;
    for (RetainedNode* current = node; current != nullptr; current = current->parent_) {
        if (current->subtree_presentation_generation_ ==
            std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "retained subtree presentation generation exhausted"
            );
        }
        ++current->subtree_presentation_generation_;
    }
    // A later declarative rebuild still needs a current retained snapshot, but an overlay-only
    // mutation must not invalidate cached widget fragments, semantics, or layout.
    invalidate_description_snapshot();
    return true;
}

bool RetainedTree::set_arrangement_value(
    const std::uint64_t identity,
    std::string name,
    runtime::Value value
) {
    RetainedNode* node = find_identity(identity);
    if (node == nullptr || !node->set_retained_value(name, value)) {
        return false;
    }
    persist_retained_value(*node, name, value);
    // Viewport state changes where children are placed, not their intrinsic size. A separate
    // ancestor revision lane invalidates only the affected arrangement-cache path.
    for (RetainedNode* current = node; current != nullptr; current = current->parent_) {
        if (current->arrangement_revision_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("retained node arrangement revision exhausted");
        }
        ++current->arrangement_revision_;
    }
    invalidate_description_snapshot();
    invalidate_layout();
    return true;
}

bool RetainedTree::field_persisted(
    const std::string_view type,
    const std::string_view name
) const {
    if (!persistence_fields_) return false;
    const std::vector<std::string> fields = persistence_fields_(type);
    return std::ranges::find(fields, name) != fields.end();
}

void RetainedTree::hydrate_persistence(RetainedNode& node) {
    if (!persistence_fields_ || !persistence_reader_) return;
    const auto key_property = node.description_->properties.find("persistenceKey");
    const runtime::Value* key_value = key_property != node.description_->properties.end()
        ? key_property->second.value() : nullptr;
    const std::string* key = key_value != nullptr ? key_value->string() : nullptr;
    if (key == nullptr || key->empty()) return;
    for (const std::string& field : persistence_fields_(node.description_->type)) {
        if (std::optional<runtime::Value> restored = persistence_reader_(
                node.description_->type, *key, field
            );
            restored.has_value()) {
            node.retained_values_.insert_or_assign(field, std::move(*restored));
        }
    }
}

void RetainedTree::persist_retained_value(
    const RetainedNode& node,
    const std::string_view name,
    const runtime::Value& value
) {
    if (!persistence_writer_ || !field_persisted(node.description_->type, name)) return;
    const auto key_property = node.description_->properties.find("persistenceKey");
    const runtime::Value* key_value = key_property != node.description_->properties.end()
        ? key_property->second.value() : nullptr;
    const std::string* key = key_value != nullptr ? key_value->string() : nullptr;
    if (key != nullptr && !key->empty()) persistence_writer_(*key, name, value);
}

void RetainedTree::clear_dirty() {
    for (const std::uint64_t identity : dirty_index_) {
        if (RetainedNode* node = find_identity(identity); node != nullptr) node->dirty_.clear();
    }
    dirty_index_.clear();
}

void RetainedTree::invalidate_description_snapshot() noexcept {
    description_snapshot_.reset();
}

void RetainedTree::consume_layout_dirty() {
    std::vector<std::uint64_t> clean;
    for (const std::uint64_t identity : dirty_index_) {
        RetainedNode* node = find_identity(identity);
        if (node == nullptr) continue;
        node->dirty_.remove(DirtyReason::structure);
        node->dirty_.remove(DirtyReason::layout);
        node->dirty_.remove(DirtyReason::text);
        node->dirty_.remove(DirtyReason::style);
        node->dirty_.remove(DirtyReason::scale);
        node->dirty_.remove(DirtyReason::editor);
        if (node->dirty_.empty()) clean.push_back(identity);
    }
    for (const std::uint64_t identity : clean) dirty_index_.erase(identity);
}

void RetainedTree::clear() {
    detach(std::move(root_), nullptr);
    invalidate_description_snapshot();
    key_index_.clear();
    identity_index_.clear();
    source_index_.clear();
    type_index_.clear();
    state_scope_index_.clear();
    semantic_index_.clear();
    virtual_index_.clear();
    dirty_index_.clear();
}

void RetainedTree::detach(std::unique_ptr<RetainedNode> node, ReconcileStats* const stats) noexcept {
    if (node == nullptr) return;
    node->lifecycle_ = RetainedLifecycle::detached;
    for (auto& child : node->children_) detach(std::move(child), stats);
    node->children_.clear();
    for (auto& cleanup : node->cleanups_) {
        try {
            cleanup();
        } catch (...) {
            /* Cleanup is a teardown boundary and cannot prevent the remainder from detaching. */
        }
    }
    if (stats != nullptr) ++stats->detached;
}

void RetainedTree::rebuild_indexes() {
    key_index_.clear();
    identity_index_.clear();
    source_index_.clear();
    type_index_.clear();
    state_scope_index_.clear();
    semantic_index_.clear();
    virtual_index_.clear();
    dirty_index_.clear();
    if (root_ != nullptr) index(*root_);
}

void RetainedTree::index(RetainedNode& node) {
    identity_index_.emplace(node.identity_, &node);
    if (node.description_->key.has_value()) {
        if (!key_index_.emplace(*node.description_->key, &node).second) {
            throw std::invalid_argument("retained tree contains duplicate key '" + *node.description_->key + "'");
        }
    }
    if (!node.description_->source_path.empty()) source_index_[node.description_->source_path].push_back(&node);
    type_index_[node.description_->type].push_back(&node);
    if (!node.description_->state_scope.empty()) state_scope_index_[node.description_->state_scope].push_back(&node);
    const auto semantics = node.description_->properties.find("semantics");
    if (semantics != node.description_->properties.end() && semantics->second.value() != nullptr &&
        semantics->second.value()->kind() != runtime::ValueKind::null_value) {
        semantic_index_.push_back(&node);
    }
    if (node.lifecycle_ == RetainedLifecycle::attached &&
        node.description_->materialization.has_value()) {
        virtual_index_.push_back(&node);
    }
    if (!node.dirty_.empty()) dirty_index_.insert(node.identity_);
    for (auto& child : node.children_) index(*child);
}

std::uint64_t RetainedTree::next_identity() {
    if (next_identity_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("retained identity space exhausted");
    }
    return ++next_identity_;
}

void RetainedTree::invalidate_layout() {
    if (layout_invalidation_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("retained layout invalidation generation exhausted");
    }
    ++layout_invalidation_generation_;
}

void RetainedTree::bump_dirty_generation(const DirtyReason reason) {
    std::uint64_t& generation = dirty_generations_[reason_index(reason)];
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("retained dirty generation exhausted");
    }
    ++generation;
}

bool expression_value_equal(
    const runtime::ExpressionValue& left,
    const runtime::ExpressionValue& right
) {
    if (left.list() != nullptr || right.list() != nullptr) {
        if (left.list() == nullptr || right.list() == nullptr ||
            (**left.list()).values.size() != (**right.list()).values.size()) {
            return false;
        }
        for (std::size_t index = 0U; index < (**left.list()).values.size(); ++index) {
            if (!expression_value_equal(
                    (**left.list()).values[index],
                    (**right.list()).values[index]
                )) {
                return false;
            }
        }
        return true;
    }
    if (left.object() != nullptr || right.object() != nullptr) {
        if (left.object() == nullptr || right.object() == nullptr ||
            (**left.object()).fields.size() != (**right.object()).fields.size()) {
            return false;
        }
        for (std::size_t index = 0U; index < (**left.object()).fields.size(); ++index) {
            const auto& left_field = (**left.object()).fields[index];
            const auto& right_field = (**right.object()).fields[index];
            if (left_field.first != right_field.first ||
                !expression_value_equal(left_field.second, right_field.second)) {
                return false;
            }
        }
        return true;
    }
    if (left.value() != nullptr || right.value() != nullptr) {
        return left.value() != nullptr && right.value() != nullptr && *left.value() == *right.value();
    }
    if (left.collection() != nullptr || right.collection() != nullptr) {
        return left.collection() != nullptr && right.collection() != nullptr &&
               runtime::collection_view_immutable_identity(**left.collection()) ==
                   runtime::collection_view_immutable_identity(**right.collection());
    }
    if (left.action() != nullptr || right.action() != nullptr) {
        return left.action() != nullptr && right.action() != nullptr &&
               action_value_equal(*left.action(), *right.action());
    }
    if (left.lambda() != nullptr || right.lambda() != nullptr) {
        if (left.lambda() == nullptr || right.lambda() == nullptr) return false;
        const runtime::LambdaValue& left_lambda = **left.lambda();
        const runtime::LambdaValue& right_lambda = **right.lambda();
        if (left_lambda.parameter != right_lambda.parameter ||
            left_lambda.body != right_lambda.body ||
            left_lambda.captured != right_lambda.captured ||
            left_lambda.captured_host_roots != right_lambda.captured_host_roots ||
            left_lambda.captured_host_dependencies !=
                right_lambda.captured_host_dependencies ||
            left_lambda.captured_lexical_dependencies !=
                right_lambda.captured_lexical_dependencies ||
            left_lambda.component_path != right_lambda.component_path ||
            left_lambda.captured_executable.size() !=
                right_lambda.captured_executable.size()) {
            return false;
        }
        auto right_capture = right_lambda.captured_executable.begin();
        for (const auto& [name, value] : left_lambda.captured_executable) {
            if (name != right_capture->first ||
                !expression_value_equal(value, right_capture->second)) {
                return false;
            }
            ++right_capture;
        }
        return true;
    }
    return true;
}

} // namespace strata::ui
