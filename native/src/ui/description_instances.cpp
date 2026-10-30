// Component instances: the identity, inputs, reads, output and effects of each described body,
// how a change reaches the instances that read it, and how their new output reaches the layers.

#include "ui/description.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "ui/description_detail.hpp"
#include "ui/widget/description.hpp"

namespace strata::ui {
namespace {

using description_detail::item_frame;
using description_detail::path_segment;
using runtime::ExpressionValue;

/** Instances kept although nothing shows them, before the least recently used are dropped. */
constexpr std::size_t maximum_unreached_instances = 512U;

using SubtreeReplacements =
    std::unordered_map<const DescriptionNode*, std::shared_ptr<const DescriptionNode>>;

/**
 * A body's output with the given subtrees replaced. The walk stays within the body's own nodes:
 * it stops at the outputs of the instances the body calls (`boundaries`) and at generated rows.
 * Null when a subtree to replace is not where the body holds it.
 */
[[nodiscard]] std::shared_ptr<const DescriptionNode>
replace_subtrees(const std::shared_ptr<const DescriptionNode>& root,
                 const SubtreeReplacements& replacements,
                 const std::unordered_set<const DescriptionNode*>& boundaries) {
    std::size_t applied = 0U;
    const auto replace = [&](const auto& self, const std::shared_ptr<const DescriptionNode>& node)
        -> std::shared_ptr<const DescriptionNode> {
        if (const auto replacement = replacements.find(node.get());
            replacement != replacements.end()) {
            ++applied;
            return replacement->second;
        }
        if (boundaries.contains(node.get()) || node->materialization.has_value() ||
            dynamic_cast<const EagerDescriptionChildren*>(node->children.get()) == nullptr) {
            return node;
        }
        bool changed = false;
        std::vector<std::shared_ptr<const DescriptionNode>> children;
        children.reserve(node->children->size());
        for (std::size_t index = 0U; index < node->children->size(); ++index) {
            const std::shared_ptr<const DescriptionNode> child = node->children->at(index);
            std::shared_ptr<const DescriptionNode> next = self(self, child);
            changed = changed || next != child;
            children.push_back(std::move(next));
        }
        if (!changed)
            return node;
        auto result = std::make_shared<DescriptionNode>(*node);
        result->children = std::make_shared<const EagerDescriptionChildren>(std::move(children));
        result->children_replaced_from = node;
        return result;
    };
    std::shared_ptr<const DescriptionNode> result = replace(replace, root);
    return applied == replacements.size() ? result : nullptr;
}

[[nodiscard]] bool same_roots(const std::shared_ptr<const runtime::HostRoots>& left,
                              const std::shared_ptr<const runtime::HostRoots>& right) {
    if (left == right)
        return true;
    static const runtime::HostRoots none;
    return (left != nullptr ? *left : none) == (right != nullptr ? *right : none);
}

template <typename Defaults>
[[nodiscard]] bool same_widget_defaults(const std::shared_ptr<const Defaults>& left,
                                        const std::shared_ptr<const Defaults>& right) {
    if (left == right)
        return true;
    if (left == nullptr || right == nullptr || left->size() != right->size())
        return false;
    const auto same_optional = [](const std::optional<ExpressionValue>& a,
                                  const std::optional<ExpressionValue>& b) {
        return a.has_value() == b.has_value() &&
               (!a.has_value() || runtime::same_expression_value(*a, *b));
    };
    return std::ranges::equal(*left, *right, [&](const auto& a, const auto& b) {
        return a.first == b.first && same_optional(a.second.style, b.second.style) &&
               same_optional(a.second.variant, b.second.variant);
    });
}

} // namespace

DescriptionBuilder::ComponentInstance*
DescriptionBuilder::find_instance(const InstanceId id) noexcept {
    const auto found = instances_.find(id);
    return found != instances_.end() ? found->second.get() : nullptr;
}

const DescriptionBuilder::ComponentInstance*
DescriptionBuilder::find_instance(const InstanceId id) const noexcept {
    const auto found = instances_.find(id);
    return found != instances_.end() ? found->second.get() : nullptr;
}

DescriptionBuilder::ComponentInstance&
DescriptionBuilder::instance(const InstanceKind kind, std::string key,
                             ComponentInstance* const parent) {
    ComponentInstance* result = nullptr;
    if (const auto found = instance_ids_.find(key); found != instance_ids_.end())
        result = find_instance(found->second);
    if (result == nullptr) {
        auto created = std::make_unique<ComponentInstance>();
        created->id = next_instance_id_++;
        created->kind = kind;
        created->key = key;
        created->dirty = true;
        result = created.get();
        instance_ids_.insert_or_assign(std::move(key), created->id);
        instances_.emplace(created->id, std::move(created));
    }
    if (parent == nullptr) {
        result->parent = 0U;
        result->depth = 0U;
        result->row = kind == InstanceKind::row ? result->id : 0U;
        result->scope_layer = kind == InstanceKind::layer ? result->id : 0U;
        return *result;
    }
    // An instance's path derives from its caller's, so its place in the tree never changes.
    result->parent = parent->id;
    result->depth = parent->depth + 1U;
    if (kind == InstanceKind::row) {
        result->row = result->id;
        result->scope_layer = 0U;
    } else {
        result->row = parent->row;
        result->scope_layer = result->row != 0U                     ? 0U
                              : parent->kind == InstanceKind::layer ? parent->id
                                                                    : parent->scope_layer;
    }
    if (parent->attached)
        attach(*result);
    return *result;
}

void DescriptionBuilder::hold_child(ComponentInstance& child) {
    Frame* const current = frame();
    if (current == nullptr)
        return;
    current->child_index.insert_or_assign(child.emitted.get(), current->children.size());
    current->children.push_back(ChildRef{child.id, child.emitted});
}

template <typename Body>
std::shared_ptr<const DescriptionNode>
DescriptionBuilder::describe_body(ComponentInstance& instance,
                                  const runtime::ExpressionScope* const outer, Body&& body) {
    Frame recording;
    recording.instance = &instance;
    recording.outer = outer;
    // The calls and function groups of the last description are what this one may keep.
    instance.previous_widget_calls = std::exchange(instance.widget_calls, {});
    instance.previous_function_groups = std::exchange(instance.function_groups, {});
    // The body's reads are its own, whatever records the call describing it.
    struct Isolated final {
        runtime::DependencyRecorder*& slot;
        runtime::DependencyRecorder* previous;
        ~Isolated() { slot = previous; }
    } isolated{recording_, std::exchange(recording_, nullptr)};
    // A row's inputs are the values of its enclosing scope it read: a read of a name the row
    // binds itself (a state, a derived value, a nested loop's item) reads no input.
    recording.expression_reads =
        runtime::DependencyRecorder(nullptr,
                                    outer != nullptr ? runtime::DependencyRecorder::Keep::first
                                                     : runtime::DependencyRecorder::Keep::none,
                                    runtime::DependencyRecorder::Keep::latest);
    recording.expression_reads.keep_only_reads_of(outer);
    frames_.push_back(&recording);
    std::shared_ptr<const DescriptionNode> root;
    try {
        root = std::forward<Body>(body)();
    } catch (...) {
        frames_.pop_back();
        throw;
    }
    frames_.pop_back();
    install_frame(instance, recording);
    return root;
}

void DescriptionBuilder::install_frame(ComponentInstance& instance, Frame& recording) {
    // The instances the body no longer calls leave with what it described before.
    std::unordered_set<InstanceId> reached;
    reached.reserve(recording.children.size());
    for (const ChildRef& child : recording.children)
        reached.insert(child.id);
    for (const ChildRef& child : instance.children) {
        if (reached.contains(child.id))
            continue;
        if (ComponentInstance* const left = find_instance(child.id);
            left != nullptr && left->parent == instance.id) {
            detach(*left);
        }
    }
    instance.children = std::move(recording.children);
    unindex_reads(instance);
    recording.reads.host = std::move(recording.expression_reads.host_reads());
    instance.reads = std::move(recording.reads);
    index_reads(instance);
    instance.outer_reads = std::move(recording.expression_reads.lexical_reads());
    instance.bindings = std::move(recording.bindings);
    instance.owned_scopes = std::move(recording.owned_scopes);
    instance.generated_slots = std::move(recording.generated_slots);
    instance.slot_templates = std::move(recording.slot_templates);
    touch(instance);
}

void DescriptionBuilder::describe(ComponentInstance& instance) {
    switch (instance.kind) {
    case InstanceKind::layer:
        describe_layer(instance);
        return;
    case InstanceKind::component:
        describe_component(instance);
        emit(instance);
        return;
    case InstanceKind::generator:
    case InstanceKind::row:
        // A generator is described by the body that calls it, a row when it is realized.
        return;
    }
}

void DescriptionBuilder::describe_layer(ComponentInstance& layer) {
    const runtime::ProgramLayer* const declaration = layer.role == runtime::LayerRole::screen
                                                         ? program().screen(layer.name)
                                                         : program().overlay(layer.name);
    if (declaration == nullptr)
        throw std::invalid_argument("requested layer declaration is not active");
    ++described_instances_;
    ++telemetry_.described_instances;
    Scope scope;
    scope.expressions.set_contextual_host_roots(contextual_host_roots_);
    scope.set_instance(declaration->declaration_scope);
    scope.declaration_scope = declaration->declaration_scope;
    layer.scope = std::move(scope);
    layer.source_path = declaration->path;
    layer.contextual_host_roots = contextual_host_roots_;
    const bool screen = layer.role == runtime::LayerRole::screen;
    std::shared_ptr<const DescriptionNode> root = describe_body(layer, nullptr, [&] {
        auto children = std::make_shared<const EagerDescriptionChildren>(
            build_block(declaration->body, layer.scope));
        ++described_nodes_;
        return DescriptionNode::create(screen ? "$screen" : "$overlay", std::nullopt,
                                       declaration->path, declaration->declaration_scope, {},
                                       std::move(children));
    });
    layer.root = share_unchanged_description(layer.root, root);
    layer.dirty = false;
    layer.settled_epoch = epoch_;
    layer.last_used_epoch = epoch_;
    emit(layer);
}

void DescriptionBuilder::describe_component(ComponentInstance& component) {
    ++described_instances_;
    ++telemetry_.described_instances;
    const runtime::ProgramComponent& declaration = program().component(component.component_index);
    std::shared_ptr<const DescriptionNode> root = describe_body(component, nullptr, [&] {
        std::vector<std::shared_ptr<const DescriptionNode>> roots =
            build_block(declaration.body, component.scope);
        if (roots.size() != 1U)
            throw std::logic_error("validated component body must describe exactly one root node");
        return std::move(roots.front());
    });
    component.root = share_unchanged_description(component.root, root);
    component.dirty = false;
    component.settled_epoch = epoch_;
    component.last_used_epoch = epoch_;
}

void DescriptionBuilder::emit(ComponentInstance& instance) {
    std::shared_ptr<const DescriptionNode> node = instance.root;
    if (node != nullptr) {
        const CallSite& site = instance.call_site;
        if (!site.projected.empty())
            node =
                description_detail::project_slot_content(node, site.component_path, site.projected);
        if (!site.behaviors.empty()) {
            auto expanded = std::make_shared<DescriptionNode>(*node);
            for (const DescriptionBehavior& behavior : site.behaviors) {
                const auto duplicate =
                    std::ranges::find(expanded->behaviors, behavior.id, &DescriptionBehavior::id);
                if (duplicate != expanded->behaviors.end())
                    throw std::logic_error("component call attaches a duplicate root behavior");
                expanded->behaviors.push_back(behavior);
            }
            node = std::move(expanded);
        }
        if (site.materialization_key.has_value() || site.loop_position.has_value()) {
            auto annotated = std::make_shared<DescriptionNode>(*node);
            if (site.materialization_key.has_value())
                annotated->materialization_key = site.materialization_key;
            if (site.loop_position.has_value()) {
                annotated->properties.insert_or_assign(
                    "$loopPosition", ExpressionValue(runtime::Value(*site.loop_position)));
            }
            node = std::move(annotated);
        }
        // Placed the same way again, the output keeps its identity.
        if (node != instance.root)
            node = share_unchanged_description(instance.emitted, node);
    }
    if (node != instance.emitted) {
        instance.emitted = std::move(node);
        emitted_changed_.push_back(instance.id);
    }
}

const std::set<std::string, std::less<>>&
DescriptionBuilder::declared_slots(ComponentInstance& instance) {
    if (instance.declared_slots_root != instance.root) {
        instance.declared_slots = instance.generated_slots;
        description_detail::declared_slot_names(instance.root,
                                                program().component(instance.component_index).path,
                                                instance.declared_slots);
        instance.declared_slots_root = instance.root;
    }
    return instance.declared_slots;
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::project_into_row(const ComponentInstance& row,
                                     std::shared_ptr<const DescriptionNode> node) const {
    // Each component the row is described within projects its caller's content into the slots
    // it declares in the row, innermost first, as its own output would have been projected.
    for (const ComponentInstance* enclosing = find_instance(row.parent); enclosing != nullptr;
         enclosing = find_instance(enclosing->parent)) {
        if (enclosing->kind == InstanceKind::component && !enclosing->generated_slots.empty() &&
            !enclosing->call_site.projected.empty()) {
            node = description_detail::project_slot_content(
                node, enclosing->call_site.component_path, enclosing->call_site.projected);
        }
    }
    return node;
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::project_into_template(const std::uint32_t component_index,
                                          std::shared_ptr<const DescriptionNode> node) {
    // Only a row describes a row template; elsewhere a template's slots are its own.
    const Frame* const current = frame();
    if (current == nullptr || current->instance->kind != InstanceKind::row)
        return node;
    const std::string& path = program().component(component_index).path;
    for (const ComponentInstance* enclosing = find_instance(current->instance->parent);
         enclosing != nullptr; enclosing = find_instance(enclosing->parent)) {
        if (enclosing->kind == InstanceKind::component &&
            enclosing->slot_templates.contains(component_index) &&
            !enclosing->call_site.projected.empty()) {
            node = description_detail::project_slot_content(node, path,
                                                            enclosing->call_site.projected);
        }
    }
    return node;
}

void DescriptionBuilder::invalidate_rows(ComponentInstance& instance) {
    const auto visit = [this](const auto& self, ComponentInstance& current) -> void {
        for (const ChildRef& child : current.children) {
            if (ComponentInstance* const called = find_instance(child.id); called != nullptr)
                self(self, *called);
        }
        for (const auto& [key, id] : current.rows) {
            static_cast<void>(key);
            if (ComponentInstance* const row = find_instance(id); row != nullptr) {
                mark_dirty(*row);
                self(self, *row);
            }
        }
    };
    visit(visit, instance);
}

void DescriptionBuilder::attach(ComponentInstance& instance) {
    if (instance.attached)
        return;
    instance.attached = true;
    touch(instance);
    if (instance.kind == InstanceKind::generator && instance.reprovision)
        reprovisioned_.push_back(instance.id);
    // The instance itself is about to be described or was found current by its caller; what it
    // called before and went stale while nothing showed it is described now that something does.
    const auto reattach = [this](ComponentInstance& descendant) {
        if (!descendant.attached && descendant.dirty)
            schedule(descendant);
        attach(descendant);
    };
    for (const ChildRef& child : instance.children) {
        if (ComponentInstance* const reached = find_instance(child.id); reached != nullptr)
            reattach(*reached);
    }
    for (const auto& [key, id] : instance.rows) {
        static_cast<void>(key);
        if (ComponentInstance* const row = find_instance(id); row != nullptr)
            reattach(*row);
    }
}

void DescriptionBuilder::detach(ComponentInstance& instance) {
    if (!instance.attached)
        return;
    instance.attached = false;
    touch(instance);
    for (const ChildRef& child : instance.children) {
        if (ComponentInstance* const left = find_instance(child.id); left != nullptr)
            detach(*left);
    }
    for (const auto& [key, id] : instance.rows) {
        static_cast<void>(key);
        if (ComponentInstance* const row = find_instance(id); row != nullptr)
            detach(*row);
    }
}

void DescriptionBuilder::touch(ComponentInstance& instance) {
    if (transaction_ != nullptr)
        transaction_->touched.push_back(instance.id);
}

void DescriptionBuilder::mark_dirty(ComponentInstance& instance) {
    if (!instance.dirty)
        ++telemetry_.invalidated_instances;
    instance.dirty = true;
    schedule(instance);
}

void DescriptionBuilder::schedule(ComponentInstance& instance) {
    if (instance.kind == InstanceKind::generator)
        return;
    if (instance.row != 0U) {
        // A row describes itself, and what it calls, when it is next realized: its generator
        // hands it out through a new provider so it is asked for again.
        ComponentInstance* const row =
            instance.kind == InstanceKind::row ? &instance : find_instance(instance.row);
        if (row == nullptr)
            return;
        row->dirty = true;
        if (ComponentInstance* const generator = find_instance(row->parent);
            generator != nullptr && !generator->reprovision) {
            generator->reprovision = true;
            reprovisioned_.push_back(generator->id);
        }
        return;
    }
    stale_.push_back(instance.id);
}

void DescriptionBuilder::release(ComponentInstance& instance) noexcept {
    for (const auto& [address, binding] : instance.bound) {
        static_cast<void>(binding);
        application_.unbind_state_scope(&instance, address.scope, address.name);
    }
    instance.bound.clear();
    uncount_scopes(instance);
}

void DescriptionBuilder::count_scopes(ComponentInstance& instance) {
    ComponentInstance* const layer = find_instance(instance.scope_layer);
    if (layer == nullptr)
        return;
    for (const std::string& scope : instance.owned_scopes)
        ++layer->layer_scope_counts[scope];
    layer->layer_scopes.reset();
    instance.counted_layer = layer->id;
    instance.counted_scopes = instance.owned_scopes;
}

void DescriptionBuilder::uncount_scopes(ComponentInstance& instance) noexcept {
    if (instance.counted_layer == 0U)
        return;
    if (ComponentInstance* const layer = find_instance(instance.counted_layer); layer != nullptr) {
        for (const std::string& scope : instance.counted_scopes) {
            if (const auto count = layer->layer_scope_counts.find(scope);
                count != layer->layer_scope_counts.end() && --count->second == 0U) {
                layer->layer_scope_counts.erase(count);
            }
        }
        layer->layer_scopes.reset();
    }
    instance.counted_layer = 0U;
    instance.counted_scopes.clear();
}

const runtime::StateScopeSet& DescriptionBuilder::layer_scopes(ComponentInstance& layer) {
    if (!layer.layer_scopes.has_value()) {
        runtime::StateScopeSet scopes;
        for (const auto& [scope, count] : layer.layer_scope_counts) {
            static_cast<void>(count);
            scopes.insert(scopes.end(), scope);
        }
        layer.layer_scopes = std::move(scopes);
    }
    return *layer.layer_scopes;
}

void DescriptionBuilder::forget(ComponentInstance& instance) {
    // What it called goes with it.
    std::vector<InstanceId> descendants;
    for (const ChildRef& child : instance.children)
        descendants.push_back(child.id);
    for (const auto& [key, id] : instance.rows) {
        static_cast<void>(key);
        descendants.push_back(id);
    }
    for (const InstanceId id : descendants) {
        if (ComponentInstance* const descendant = find_instance(id);
            descendant != nullptr && descendant->parent == instance.id) {
            forget(*descendant);
        }
    }
    release(instance);
    unindex_reads(instance);
    if (instance.kind == InstanceKind::row) {
        if (ComponentInstance* const generator = find_instance(instance.parent);
            generator != nullptr) {
            std::erase_if(generator->rows,
                          [&instance](const auto& row) { return row.second == instance.id; });
        }
    }
    const InstanceId id = instance.id;
    instance_ids_.erase(instance.key);
    instances_.erase(id);
}

void DescriptionBuilder::index_reads(const ComponentInstance& instance) {
    for (const auto& [address, value] : instance.reads.state) {
        static_cast<void>(value);
        state_readers_[address].insert(instance.id);
    }
    for (const auto& [canonical, dependency] : instance.reads.host) {
        // A surface's own roots are compared as a whole when they change.
        if (!dependency.contextual)
            host_readers_[canonical].insert(instance.id);
    }
    for (const RetainedRead& read : instance.reads.retained)
        retained_readers_[read.name].insert(instance.id);
    if (instance.reads.raised_errors)
        erroring_.insert(instance.id);
}

void DescriptionBuilder::unindex_reads(const ComponentInstance& instance) noexcept {
    const auto remove = [&instance](auto& index, const auto& key) {
        if (const auto found = index.find(key); found != index.end()) {
            found->second.erase(instance.id);
            if (found->second.empty())
                index.erase(found);
        }
    };
    for (const auto& [address, value] : instance.reads.state) {
        static_cast<void>(value);
        remove(state_readers_, address);
    }
    for (const auto& [canonical, dependency] : instance.reads.host) {
        static_cast<void>(dependency);
        remove(host_readers_, canonical);
    }
    for (const RetainedRead& read : instance.reads.retained)
        remove(retained_readers_, read.name);
    erroring_.erase(instance.id);
}

bool DescriptionBuilder::retained_reads_current(const ComponentInstance& instance) const {
    return std::ranges::all_of(instance.reads.retained, [this](const RetainedRead& read) {
        const RetainedDescriptionSnapshot::Node* const retained = retained_widget(read.query);
        const runtime::Value* const current =
            retained != nullptr ? retained->retained_value(read.name) : nullptr;
        return current == nullptr ? !read.value.has_value()
                                  : read.value.has_value() && *current == *read.value;
    });
}

void DescriptionBuilder::collect_invalidations() {
    const std::uint64_t host_count = application_.host().invalidation_count();
    const auto checked = [&] {
        changed_states_.clear();
        all_states_changed_ = false;
        checked_host_count_ = host_count;
        checked_retained_snapshot_ = retained_snapshot_;
    };
    if (contextual_host_roots_changed_) {
        // Every scope holds the surface's roots: every body is described in the new ones.
        contextual_host_roots_changed_ = false;
        for (const auto& [id, instance] : instances_) {
            static_cast<void>(id);
            if (instance->kind != InstanceKind::generator)
                mark_dirty(*instance);
        }
        checked();
        return;
    }
    // Only the readers of what changed are looked at, and only those whose value differs now
    // are stale.
    const auto stale_by = [this](const Readers& readers, const auto& changed) {
        std::vector<ComponentInstance*> stale;
        for (const InstanceId id : readers) {
            if (ComponentInstance* const reader = find_instance(id);
                reader != nullptr && !reader->dirty && changed(*reader)) {
                stale.push_back(reader);
            }
        }
        for (ComponentInstance* const reader : stale)
            mark_dirty(*reader);
    };
    const std::vector<InstanceId> erroring(erroring_.begin(), erroring_.end());
    for (const InstanceId id : erroring) {
        if (ComponentInstance* const reader = find_instance(id); reader != nullptr)
            mark_dirty(*reader);
    }
    const auto state_changed = [this, &stale_by](const runtime::StateAddress& address,
                                                 const Readers& readers) {
        const runtime::Value* const current = application_.state().find(address);
        stale_by(readers, [&](const ComponentInstance& reader) {
            const auto read = reader.reads.state.find(address);
            return current == nullptr || read == reader.reads.state.end() ||
                   *current != read->second;
        });
    };
    if (all_states_changed_) {
        for (const auto& [address, readers] : state_readers_)
            state_changed(address, readers);
    } else {
        for (const runtime::StateAddress& address : changed_states_) {
            if (const auto readers = state_readers_.find(address); readers != state_readers_.end())
                state_changed(address, readers->second);
        }
    }
    if (host_count != checked_host_count_) {
        for (const auto& [canonical, readers] : host_readers_) {
            stale_by(readers, [&](const ComponentInstance& reader) {
                const auto read = reader.reads.host.find(canonical);
                return read == reader.reads.host.end() ||
                       !host_read_current(canonical, read->second);
            });
        }
    }
    if (retained_snapshot_ != checked_retained_snapshot_) {
        Readers readers;
        for (const auto& [name, named] : retained_readers_) {
            static_cast<void>(name);
            readers.insert(named.begin(), named.end());
        }
        stale_by(readers, [this](const ComponentInstance& reader) {
            return !retained_reads_current(reader);
        });
    }
    checked();
}

void DescriptionBuilder::describe_stale() {
    using Entry = std::pair<std::uint32_t, InstanceId>;
    std::set<Entry> queue;
    const auto take = [&] {
        for (const InstanceId id : stale_) {
            const ComponentInstance* const stale = find_instance(id);
            if (stale != nullptr && stale->dirty && stale->attached && stale->row == 0U &&
                (stale->kind == InstanceKind::layer || stale->kind == InstanceKind::component)) {
                queue.emplace(stale->depth, id);
            }
        }
        stale_.clear();
    };
    take();
    // Shallowest first: a body described again calls its components with what it passes now,
    // and those it calls with other inputs are described with it.
    while (!queue.empty()) {
        const Entry next = *queue.begin();
        queue.erase(queue.begin());
        if (ComponentInstance* const stale = find_instance(next.second);
            stale != nullptr && stale->dirty && stale->attached) {
            describe(*stale);
        }
        take();
    }
}

void DescriptionBuilder::flush_reprovisions() {
    std::vector<InstanceId> pending = std::exchange(reprovisioned_, {});
    for (const InstanceId id : pending) {
        ComponentInstance* const generator = find_instance(id);
        if (generator == nullptr || !generator->reprovision || !generator->attached)
            continue;
        generator->reprovision = false;
        // A generator its caller described this build already hands out a new provider.
        if (generator->settled_epoch != epoch_)
            reprovision(*generator);
    }
}

void DescriptionBuilder::reprovision(ComponentInstance& generator) {
    if (generator.root == nullptr || generator.rows_source == nullptr)
        return;
    auto node = std::make_shared<DescriptionNode>(*generator.root);
    node->children = provide(generator, generator.rows_source);
    node->children_replaced_from = generator.root;
    generator.root = std::move(node);
    generator.settled_epoch = epoch_;
    ++telemetry_.patched_instances;
    emit(generator);
}

bool DescriptionBuilder::patch(ComponentInstance& instance) {
    SubtreeReplacements replacements;
    std::unordered_set<const DescriptionNode*> boundaries;
    boundaries.reserve(instance.children.size());
    for (const ChildRef& child : instance.children) {
        boundaries.insert(child.seen.get());
        const ComponentInstance* const called = find_instance(child.id);
        if (called != nullptr && called->emitted != child.seen)
            replacements.emplace(child.seen.get(), called->emitted);
    }
    if (replacements.empty())
        return true;
    std::shared_ptr<const DescriptionNode> patched =
        replace_subtrees(instance.root, replacements, boundaries);
    if (patched == nullptr)
        return false;
    for (ChildRef& child : instance.children) {
        if (const ComponentInstance* const called = find_instance(child.id); called != nullptr)
            child.seen = called->emitted;
    }
    instance.root = std::move(patched);
    ++telemetry_.patched_instances;
    emit(instance);
    return true;
}

void DescriptionBuilder::patch_ancestors() {
    using Entry = std::pair<std::uint32_t, InstanceId>;
    std::set<Entry, std::greater<>> queue;
    const auto take = [&] {
        for (const InstanceId id : emitted_changed_) {
            const ComponentInstance* const changed = find_instance(id);
            if (changed == nullptr || !changed->attached || changed->parent == 0U)
                continue;
            if (const ComponentInstance* const parent = find_instance(changed->parent);
                parent != nullptr) {
                queue.emplace(parent->depth, parent->id);
            }
        }
        emitted_changed_.clear();
    };
    take();
    // Deepest first, so a body takes all of its components' new outputs at once.
    while (!queue.empty()) {
        const Entry next = *queue.begin();
        queue.erase(queue.begin());
        ComponentInstance* const parent = find_instance(next.second);
        if (parent == nullptr || !parent->attached) {
            take();
            continue;
        }
        const bool outdated = std::ranges::any_of(parent->children, [this](const ChildRef& child) {
            const ComponentInstance* const called = find_instance(child.id);
            return called != nullptr && called->emitted != child.seen;
        });
        if (outdated) {
            switch (parent->kind) {
            case InstanceKind::row:
                // A row is described again, with what it calls, when it is next realized.
                mark_dirty(*parent);
                flush_reprovisions();
                break;
            case InstanceKind::generator:
                break;
            case InstanceKind::layer:
            case InstanceKind::component:
                if (!patch(*parent))
                    describe(*parent);
                break;
            }
        }
        take();
    }
}

void DescriptionBuilder::commit(Transaction& transaction) {
    runtime::StateStore& state = application_.state();
    for (const StateInitialization& initialization : transaction.states) {
        if (initialization.persisted.has_value() && state.find(initialization.address) == nullptr) {
            static_cast<void>(state.write(initialization.address, initialization.slot,
                                          *initialization.persisted));
        }
        static_cast<void>(state.read(initialization.address, initialization.slot));
    }
    for (const std::string& key : transaction.erased_application_values)
        static_cast<void>(application_.durability().erase_application_value(key));
    for (const auto& [key, field] : transaction.erased_widget_values)
        static_cast<void>(application_.durability().erase_widget_value(key, field));
    for (runtime::RuntimeDiagnostic& report : transaction.reports)
        application_.services().report(std::move(report));

    // What instances no longer bind or own is withdrawn first, so a scope one instance gives up
    // can be bound again by another.
    for (const InstanceId id : transaction.touched) {
        ComponentInstance* const touched = find_instance(id);
        if (touched == nullptr)
            continue;
        for (auto bound = touched->bound.begin(); bound != touched->bound.end();) {
            const auto wanted =
                touched->attached ? touched->bindings.find(bound->first) : touched->bindings.end();
            if (wanted != touched->bindings.end() && wanted->second == bound->second) {
                ++bound;
                continue;
            }
            application_.unbind_state_scope(touched, bound->first.scope, bound->first.name);
            bound = touched->bound.erase(bound);
        }
        const InstanceId layer = touched->attached ? touched->scope_layer : 0U;
        if (touched->counted_layer != layer || touched->counted_scopes != touched->owned_scopes)
            uncount_scopes(*touched);
    }
    for (const InstanceId id : transaction.touched) {
        ComponentInstance* const touched = find_instance(id);
        if (touched == nullptr || !touched->attached)
            continue;
        for (const auto& [address, binding] : touched->bindings) {
            if (touched->bound.contains(address))
                continue;
            application_.bind_state_scope(touched, address.scope, address.name,
                                          binding.declaration_scope, binding.address_scope);
            touched->bound.emplace(address, binding);
        }
        for (const std::string& scope : touched->owned_scopes)
            state.mark_owned_scope(scope);
        if (touched->scope_layer != 0U && touched->counted_layer == 0U)
            count_scopes(*touched);
    }
    transaction.touched.clear();
}

void DescriptionBuilder::evict() {
    if (instances_.size() <= eviction_threshold_)
        return;
    // Instances nothing shows, and realized rows nothing holds any longer, least recent first.
    std::vector<std::pair<std::uint64_t, InstanceId>> candidates;
    for (const auto& [id, instance] : instances_) {
        const bool unheld_row =
            instance->kind == InstanceKind::row && !instance->dirty &&
            instance->emitted != nullptr &&
            instance->emitted.use_count() <= (instance->root == instance->emitted ? 2 : 1);
        if (!instance->attached || unheld_row)
            candidates.emplace_back(instance->last_used_epoch, id);
    }
    if (candidates.size() > maximum_unreached_instances) {
        std::ranges::sort(candidates);
        const std::size_t removals = candidates.size() - maximum_unreached_instances;
        for (std::size_t index = 0U; index < removals; ++index) {
            if (ComponentInstance* const evicted = find_instance(candidates[index].second);
                evicted != nullptr && (!evicted->attached || evicted->kind == InstanceKind::row)) {
                forget(*evicted);
            }
        }
    }
    eviction_threshold_ = std::max<std::size_t>(1024U, instances_.size() * 2U);
}

void DescriptionBuilder::reset() noexcept {
    for (const auto& [id, instance] : instances_) {
        static_cast<void>(id);
        for (const auto& [address, binding] : instance->bound) {
            static_cast<void>(binding);
            application_.unbind_state_scope(instance.get(), address.scope, address.name);
        }
    }
    instances_.clear();
    instance_ids_.clear();
    layers_.clear();
    state_readers_.clear();
    host_readers_.clear();
    retained_readers_.clear();
    erroring_.clear();
    stale_.clear();
    emitted_changed_.clear();
    reprovisioned_.clear();
}

DescriptionLayersBuildResult
DescriptionBuilder::build_layers(const std::span<const LayerDescriptionRequest> layers) {
    if (layers.empty())
        throw std::invalid_argument("description layer list must not be empty");
    const std::shared_ptr<const runtime::RuntimeUnit>& active = application_.active_unit();
    if (active == nullptr)
        throw std::logic_error("description build requires an active runtime unit");
    if (epoch_ == std::numeric_limits<std::uint64_t>::max())
        use_unit(nullptr);
    use_unit(active);
    ++epoch_;
    ++telemetry_.builds;
    diagnostics_.clear();
    evaluated_expressions_ = 0U;
    described_nodes_ = 0U;
    described_instances_ = 0U;
    Transaction transaction;
    transaction_ = &transaction;
    std::vector<ComponentInstance*> requested;
    requested.reserve(layers.size());
    try {
        collect_invalidations();
        for (const LayerDescriptionRequest& layer : layers) {
            const runtime::ProgramLayer* const declaration =
                layer.role == runtime::LayerRole::screen ? program().screen(layer.name)
                                                         : program().overlay(layer.name);
            if (declaration == nullptr)
                throw std::invalid_argument("requested layer declaration is not active");
            ComponentInstance& described = instance(
                InstanceKind::layer,
                std::string(layer.role == runtime::LayerRole::screen ? "screen\n" : "overlay\n") +
                    layer.name,
                nullptr);
            described.role = layer.role;
            described.name = layer.name;
            if (described.root == nullptr || described.source_path != declaration->path ||
                !same_contextual_host_roots(described.contextual_host_roots)) {
                described.dirty = true;
            }
            requested.push_back(&described);
        }
        // Layers no longer shown leave, with everything they show; they keep their instances.
        for (const InstanceId id : layers_) {
            if (std::ranges::none_of(
                    requested, [id](const ComponentInstance* layer) { return layer->id == id; })) {
                if (ComponentInstance* const left = find_instance(id); left != nullptr)
                    detach(*left);
            }
        }
        layers_.clear();
        for (ComponentInstance* const layer : requested) {
            layers_.push_back(layer->id);
            attach(*layer);
            if (layer->dirty)
                stale_.push_back(layer->id);
        }
        // Describing a body can reach cached instances that went stale while nothing showed
        // them, and patching can describe a body: settle until nothing is left.
        do {
            describe_stale();
            flush_reprovisions();
            patch_ancestors();
            flush_reprovisions();
        } while (!stale_.empty() || !emitted_changed_.empty());
        commit(transaction);
    } catch (...) {
        // A build that failed part way leaves nothing it described: the next starts afresh.
        transaction_ = nullptr;
        reset();
        throw;
    }
    transaction_ = nullptr;
    DescriptionLayersBuildResult result;
    result.roots.reserve(requested.size());
    result.layer_state_scopes.reserve(requested.size());
    for (ComponentInstance* const layer : requested) {
        result.roots.push_back(layer->emitted);
        result.layer_state_scopes.push_back(layer_scopes(*layer));
    }
    result.diagnostics = std::move(diagnostics_);
    result.evaluated_expressions = evaluated_expressions_;
    result.described_nodes = described_nodes_;
    result.described_instances = described_instances_;
    evict();
    return result;
}

bool DescriptionBuilder::observes_retained_value(const RetainedNode& node,
                                                 const std::string_view name) const {
    if (retained_snapshot_ == nullptr || instances_.empty())
        return true;
    // Only the bodies that read a value of this name, on this node, are described again.
    const auto readers = retained_readers_.find(name);
    if (readers == retained_readers_.end())
        return false;
    return std::ranges::any_of(readers->second, [&](const InstanceId id) {
        const ComponentInstance* const reader = find_instance(id);
        return reader != nullptr &&
               std::ranges::any_of(reader->reads.retained, [&](const RetainedRead& read) {
                   if (read.name != name)
                       return false;
                   const RetainedDescriptionSnapshot::Node* const retained =
                       retained_widget(read.query);
                   return retained != nullptr && retained->identity == node.identity();
               });
    });
}

DescriptionBuilder::ComponentInstance&
DescriptionBuilder::generator(const Scope& scope, const runtime::ProgramCall& call,
                              const std::optional<std::string>& key) {
    body_effect();
    std::string identity = "generator\n" + scope.instance_path() + "\n" + call.path;
    if (key.has_value())
        identity += "\n" + *key;
    return instance(InstanceKind::generator, std::move(identity),
                    frame() != nullptr ? frame()->instance : nullptr);
}

std::shared_ptr<const DescriptionChildren>
DescriptionBuilder::provide(ComponentInstance& generator, std::shared_ptr<RowSource> source) {
    if (source->builder == nullptr) {
        source->builder = self_;
        source->generator = generator.id;
        source->application = &application_;
        source->widgets = widget_snapshot();
        source->unit = unit_;
        source->retained = retained_snapshot_;
    }
    generator.rows_source = source;
    std::shared_ptr<const RowSource> rows = std::move(source);
    const std::size_t count = rows->count;
    return std::make_shared<const GeneratedDescriptionChildren>(
        count, [rows = std::move(rows)](const std::size_t index) { return realize(rows, index); });
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::realize(const std::shared_ptr<const RowSource>& source,
                            const std::size_t index) {
    if (DescriptionBuilder* const builder = source->builder != nullptr ? *source->builder : nullptr;
        builder != nullptr && builder->unit_ == source->unit) {
        if (const ComponentInstance* const generator = builder->find_instance(source->generator);
            generator != nullptr && generator->kind == InstanceKind::generator) {
            return builder->realize_row(source, index);
        }
    }
    // The builder that made the provider is gone, describes another unit, or dropped the
    // generator: the row is described by a builder of its own, as that builder would have.
    DescriptionBuilder alone(*source->application, *source->widgets);
    alone.widget_snapshot_ = source->widgets;
    alone.widget_snapshot_revision_ = source->widgets->revision();
    alone.use_unit(source->unit);
    alone.contextual_host_roots_ = source->scope.expressions.shared_contextual_host_roots();
    alone.retained_snapshot_ = source->retained;
    ComponentInstance& generator = alone.instance(
        InstanceKind::generator, "generator\n" + source->scope.instance_path(), nullptr);
    generator.attached = true;
    auto rows = std::make_shared<RowSource>(*source);
    rows->builder = alone.self_;
    rows->generator = generator.id;
    generator.rows_source = rows;
    return alone.realize_row(rows, index);
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::realize_row(const std::shared_ptr<const RowSource>& source,
                                const std::size_t index) {
    // A row is realized whenever its provider is asked, within a build or between builds: it is
    // a transaction of its own, which leaves the builder's current one as it was.
    struct Realization final {
        explicit Realization(DescriptionBuilder& owner)
            : builder(owner), frames(std::exchange(owner.frames_, {})),
              diagnostics(std::exchange(owner.diagnostics_, {})),
              evaluated_expressions(std::exchange(owner.evaluated_expressions_, 0U)),
              described_nodes(std::exchange(owner.described_nodes_, 0U)),
              described_instances(owner.described_instances_),
              emitted_changed(std::exchange(owner.emitted_changed_, {})),
              outer_transaction(std::exchange(owner.transaction_, &transaction)),
              recording(std::exchange(owner.recording_, nullptr)) {}
        ~Realization() {
            builder.frames_ = std::move(frames);
            builder.diagnostics_ = std::move(diagnostics);
            builder.evaluated_expressions_ = evaluated_expressions;
            builder.described_nodes_ = described_nodes;
            builder.described_instances_ = described_instances;
            // Whatever the row called took its outputs as the row was described.
            builder.emitted_changed_ = std::move(emitted_changed);
            builder.transaction_ = outer_transaction;
            builder.recording_ = recording;
        }
        Realization(const Realization&) = delete;
        Realization& operator=(const Realization&) = delete;

        DescriptionBuilder& builder;
        Transaction transaction;
        std::vector<Frame*> frames;
        std::vector<runtime::RuntimeDiagnostic> diagnostics;
        std::size_t evaluated_expressions;
        std::size_t described_nodes;
        std::size_t described_instances;
        std::vector<InstanceId> emitted_changed;
        Transaction* outer_transaction;
        runtime::DependencyRecorder* recording;
    } realization(*this);

    ComponentInstance* const owner = find_instance(source->generator);
    std::string row_key = source->kind == RowSource::Kind::repeater
                              ? source->sequence->key_at(index)
                              : std::to_string(index);
    ComponentInstance& row =
        instance(InstanceKind::row, "row\n" + std::to_string(owner->id) + "\n" + row_key, owner);
    owner->rows.insert_or_assign(std::move(row_key), row.id);
    std::shared_ptr<const DescriptionNode> result;
    if (row_current(row, *source, index)) {
        row.row_source = source;
        row.row_index = index;
        row.last_used_epoch = epoch_;
        ++telemetry_.reused_rows;
        result = row.emitted;
    } else {
        result = describe_row(row, source, index);
    }
    commit(realization.transaction);
    return result;
}

bool DescriptionBuilder::row_current(const ComponentInstance& row, const RowSource& source,
                                     const std::size_t index) const {
    if (row.root == nullptr || row.dirty || row.row_source == nullptr)
        return false;
    const RowSource& previous = *row.row_source;
    if (&previous == &source)
        return row.row_index == index;
    // A widget's rows come from a factory nothing compares.
    if (source.kind != RowSource::Kind::repeater || previous.kind != RowSource::Kind::repeater ||
        source.unit != previous.unit || source.block != previous.block ||
        source.item != previous.item || source.index != previous.index ||
        source.scope.instance_path() != previous.scope.instance_path() ||
        !same_roots(source.scope.expressions.shared_contextual_host_roots(),
                    previous.scope.expressions.shared_contextual_host_roots()) ||
        !same_widget_defaults(source.scope.widget_defaults, previous.scope.widget_defaults)) {
        return false;
    }
    // A row of another generation is the same row while what it read of its item, its index and
    // the enclosing scope reads the same.
    runtime::ExpressionScope probe = source.scope.expressions;
    probe.push(item_frame(source.item, source.sequence->item_at(index), source.index,
                          source.sequence->source_index_at(index)));
    return std::ranges::all_of(row.outer_reads, [&probe](const auto& read) {
        return runtime::lexical_read_current(probe, read.first, read.second.value);
    });
}

runtime::StateScopeSet DescriptionBuilder::row_owned_scopes(const ComponentInstance& row) const {
    runtime::StateScopeSet scopes;
    const auto collect = [&](const auto& self, const ComponentInstance& instance) -> void {
        scopes.insert(instance.owned_scopes.begin(), instance.owned_scopes.end());
        // A nested generator's rows publish their own scopes.
        for (const ChildRef& child : instance.children) {
            if (const ComponentInstance* const called = find_instance(child.id); called != nullptr)
                self(self, *called);
        }
    };
    collect(collect, row);
    return scopes;
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::describe_row(ComponentInstance& row,
                                 const std::shared_ptr<const RowSource>& source,
                                 const std::size_t index) {
    std::shared_ptr<const DescriptionNode> described;
    std::string materialization_key;
    std::size_t synthesized_nodes = 0U;
    if (source->kind == RowSource::Kind::repeater) {
        const runtime::IndexableSequence& sequence = *source->sequence;
        const std::string canonical_key = sequence.key_at(index);
        Scope item_scope = source->scope;
        item_scope.expressions.push(item_frame(source->item, sequence.item_at(index), source->index,
                                               sequence.source_index_at(index)));
        // The row's root key already names it among the rows: keyed calls inside stay in it.
        item_scope.set_instance(item_scope.instance_path() + "/" +
                                std::string(source->item.name()) + ":" +
                                path_segment(canonical_key));
        row.scope = std::move(item_scope);
        described = describe_body(row, &row.scope.expressions, [&] {
            std::vector<std::shared_ptr<const DescriptionNode>> nodes =
                build_block(source->block, row.scope);
            if (nodes.size() != 1U) {
                throw std::logic_error("validated Repeater identity selected a row body that did "
                                       "not produce one root");
            }
            return project_into_row(row, std::move(nodes.front()));
        });
        auto keyed = std::make_shared<DescriptionNode>(*described);
        keyed->key = canonical_key;
        described = std::move(keyed);
        materialization_key = canonical_key;
    } else {
        row.scope = source->scope;
        WidgetDescriptionExpansion item;
        WidgetDescriptionScope item_scope(
            item, source->scope.instance_path(), application_.bundle()->action_registry(),
            *source->widgets, nullptr,
            [this, &source](const std::string_view component, std::string key,
                            WidgetTemplateArguments arguments) {
                return build_component_template(component, std::move(key), std::move(arguments),
                                                source->scope);
            },
            {});
        described = describe_body(row, nullptr, [&] { return source->factory(item_scope, index); });
        if (described == nullptr)
            throw std::logic_error("generated row evaluator returned a null description");
        materialization_key = described->key.has_value()
                                  ? *described->key
                                  : "widget-generated-" + std::to_string(index);
        synthesized_nodes = item.synthesized_nodes;
    }

    // The row's work and diagnostics travel with it: it is described again only when what it
    // read changes, and is published once.
    auto anchored = std::make_shared<DescriptionNode>(*described);
    if (!anchored->materialization_key.has_value())
        anchored->materialization_key = std::move(materialization_key);
    DescriptionMaterialization result{
        row_owned_scopes(row),
        std::move(diagnostics_),
        evaluated_expressions_,
        described_nodes_,
    };
    diagnostics_.clear();
    if (synthesized_nodes > std::numeric_limits<std::size_t>::max() - result.described_nodes)
        throw std::overflow_error("generated row described-node count exhausted");
    result.described_nodes += synthesized_nodes;
    if (anchored->materialization_result != nullptr) {
        const DescriptionMaterialization& nested = *anchored->materialization_result;
        result.owned_state_scopes.insert(nested.owned_state_scopes.begin(),
                                         nested.owned_state_scopes.end());
        result.diagnostics.insert(result.diagnostics.end(), nested.diagnostics.begin(),
                                  nested.diagnostics.end());
        if (nested.evaluated_expressions >
                std::numeric_limits<std::size_t>::max() - result.evaluated_expressions ||
            nested.described_nodes >
                std::numeric_limits<std::size_t>::max() - result.described_nodes) {
            throw std::overflow_error("nested generated row counters exhausted");
        }
        result.evaluated_expressions += nested.evaluated_expressions;
        result.described_nodes += nested.described_nodes;
    }
    anchored->materialization_result =
        std::make_shared<const DescriptionMaterialization>(std::move(result));
    row.root = anchored;
    row.emitted = std::move(anchored);
    row.row_source = source;
    row.row_index = index;
    row.dirty = false;
    row.settled_epoch = epoch_;
    row.last_used_epoch = epoch_;
    ++telemetry_.described_rows;
    return row.emitted;
}

} // namespace strata::ui
