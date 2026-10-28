#include "ui/description.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/description_detail.hpp"
#include "ui/widget/description.hpp"

namespace strata::ui {
namespace {

using runtime::ExpressionValue;
using runtime::Symbol;

[[nodiscard]] std::optional<std::string> key_from_value(const ExpressionValue& value) {
    const runtime::Value* scalar = value.value();
    if (scalar == nullptr || scalar->kind() == runtime::ValueKind::null_value)
        return std::nullopt;
    if (scalar->key() != nullptr)
        return scalar->key()->value;
    if (scalar->string() != nullptr && !scalar->string()->empty())
        return *scalar->string();
    if (scalar->number() != nullptr)
        return runtime::display_string(*scalar);
    return std::nullopt;
}

using description_detail::item_frame;
using description_detail::path_segment;

[[nodiscard]] std::string component_instance_path(const std::string_view parent,
                                                  const std::string_view type,
                                                  const std::optional<std::string>& key,
                                                  const std::string_view source_path) {
    const std::string identity =
        key.has_value() ? "key:" + path_segment(*key) : "call:" + std::string(source_path);
    return std::string(parent) + "/component:" + std::string(type) + "/" + identity;
}

[[nodiscard]] std::string repeated_item_segment(const runtime::Value& value,
                                                const std::size_t index) {
    const runtime::Value* stable = value.field("key");
    if (stable == nullptr)
        stable = value.field("id");
    if (stable == nullptr && value.kind() != runtime::ValueKind::list &&
        value.kind() != runtime::ValueKind::object &&
        value.kind() != runtime::ValueKind::null_value) {
        stable = &value;
    }
    return stable != nullptr ? runtime::display_string(*stable) : std::to_string(index);
}

[[nodiscard]] std::string lazy_item_key(const runtime::Value& value, const std::size_t index) {
    const runtime::Value* stable = value.field("key");
    if (stable == nullptr)
        stable = value.field("id");
    if (stable != nullptr) {
        if (stable->key() != nullptr && !stable->key()->value.empty())
            return stable->key()->value;
        if (stable->string() != nullptr && !stable->string()->empty())
            return *stable->string();
    }
    return "dsl-lazy-" + std::to_string(index);
}

/** A binding as a declaration or parameter makes it: data drops the state binding it was read
 * with, and the name's state binding becomes `binding`, or none. */
[[nodiscard]] runtime::ScopeBinding
declared_binding(const Symbol name, const ExpressionValue& value,
                 std::shared_ptr<const runtime::LexicalStateBinding> binding) {
    const runtime::ScopeStateBinding state = binding != nullptr
                                                 ? runtime::ScopeStateBinding::bound
                                                 : runtime::ScopeStateBinding::cleared;
    return runtime::ScopeBinding{name, true, state,
                                 value.executable() ? value : value.without_state_binding(),
                                 std::move(binding)};
}

struct TransparentStringHash final {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(const std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

[[nodiscard]] bool same_inputs(const std::vector<ExpressionValue>& left,
                               const std::vector<ExpressionValue>& right) {
    return std::ranges::equal(left, right, runtime::same_expression_value);
}

/**
 * One Repeater generation: its items and their root keys. A generation evaluates every key once
 * (it must, to reject duplicates), so it keeps them, and a key's index, instead of evaluating them
 * again whenever virtualization asks.
 */
class RepeaterIndexableSequence final : public runtime::IndexableSequence {
  public:
    using KeyIndexes = std::unordered_map<std::string, std::size_t, TransparentStringHash,
                                          std::equal_to<>>;

    RepeaterIndexableSequence(const std::uint64_t generation, runtime::Value source,
                              std::optional<std::vector<std::size_t>> selection,
                              std::vector<std::string> keys, KeyIndexes indexes)
        : generation_(generation), source_(std::move(source)), selection_(std::move(selection)),
          keys_(std::move(keys)), indexes_(std::move(indexes)) {
        if (source_.list() == nullptr)
            throw std::invalid_argument("repeater sequence source must be a list");
        if (keys_.size() != count() || indexes_.size() != keys_.size())
            throw std::invalid_argument("repeater sequence requires one distinct key per item");
    }

    [[nodiscard]] std::uint64_t generation() const noexcept override {
        return generation_;
    }

    [[nodiscard]] std::size_t count() const noexcept override {
        return selection_.has_value() ? selection_->size() : source_.list()->values.size();
    }

    [[nodiscard]] const runtime::Value& item_at(const std::size_t index) const override {
        return source_.list()->values.at(source_index_at(index));
    }

    [[nodiscard]] std::size_t source_index_at(const std::size_t index) const override {
        if (index >= count())
            throw std::out_of_range("repeater sequence index is outside the selection");
        return selection_.has_value() ? selection_->at(index) : index;
    }

    [[nodiscard]] std::string key_at(const std::size_t index) const override {
        if (index >= keys_.size())
            throw std::out_of_range("repeater sequence index is outside the selection");
        return keys_[index];
    }

    [[nodiscard]] std::optional<std::size_t>
    index_of_key(const std::string_view key) const override {
        const auto found = indexes_.find(key);
        return found != indexes_.end() ? std::optional<std::size_t>(found->second) : std::nullopt;
    }

  private:
    std::uint64_t generation_;
    runtime::Value source_;
    std::optional<std::vector<std::size_t>> selection_;
    std::vector<std::string> keys_;
    KeyIndexes indexes_;
};

/**
 * The reads a Repeater generation depends on. The enclosing body is told them too: the generation
 * is part of that body's output, so the body is stale when any of these reads change.
 */
struct RepeaterExpressionDependencies final : runtime::ExpressionDependencyObserver {
    explicit RepeaterExpressionDependencies(runtime::ExpressionDependencyObserver* const enclosing)
        : upstream(enclosing) {}

    void exclude(const Symbol name) {
        excluded.push_back(name);
    }

    void lexical(const Symbol name, const ExpressionValue& value) override {
        if (std::ranges::find(excluded, name) != excluded.end())
            return;
        lexical_values.insert_or_assign(name, runtime::capture_expression_dependency(value));
        if (upstream != nullptr)
            upstream->lexical(name, value);
    }

    void host(const runtime::ExpressionHostDependency& dependency) override {
        host_values.insert_or_assign(runtime::canonical_host_dependency_path(dependency.path),
                                     dependency);
        if (upstream != nullptr)
            upstream->host(dependency);
    }

    runtime::ExpressionDependencyObserver* upstream = nullptr;
    std::vector<Symbol> excluded;
    std::map<Symbol, runtime::ExpressionDependencyValue> lexical_values;
    std::map<std::string, runtime::ExpressionHostDependency, std::less<>> host_values;
};

[[nodiscard]] bool repeater_dependencies_current(const DescriptionSequenceGeneration& previous,
                                                 const runtime::ExpressionScope& scope,
                                                 const runtime::ExpressionRuntime& expressions) {
    for (const auto& [name, value] : previous.lexical_dependencies) {
        const ExpressionValue* current = scope.find(name);
        if (current == nullptr || !runtime::same_expression_value(*current, value.value))
            return false;
    }
    for (const auto& [canonical, dependency] : previous.host_dependencies) {
        static_cast<void>(canonical);
        if (expressions.read_host_dependency(dependency.path, scope) != dependency)
            return false;
    }
    return true;
}

class ExpressionDependencyObserverRestore final {
  public:
    ExpressionDependencyObserverRestore(runtime::ExpressionRuntime& expressions,
                                        runtime::ExpressionDependencyObserver* observer)
        : expressions_(expressions), previous_(expressions.exchange_dependency_observer(observer)) {
    }

    ~ExpressionDependencyObserverRestore() {
        static_cast<void>(expressions_.exchange_dependency_observer(previous_));
    }

    ExpressionDependencyObserverRestore(const ExpressionDependencyObserverRestore&) = delete;
    ExpressionDependencyObserverRestore&
    operator=(const ExpressionDependencyObserverRestore&) = delete;

    [[nodiscard]] runtime::ExpressionDependencyObserver* previous() const noexcept {
        return previous_;
    }

  private:
    runtime::ExpressionRuntime& expressions_;
    runtime::ExpressionDependencyObserver* previous_;
};

/** The host values one style resolution read. */
struct StyleDependencies final : runtime::ExpressionDependencyObserver {
    void lexical(Symbol, const ExpressionValue&) override {}

    void host(const runtime::ExpressionHostDependency& dependency) override {
        host_values.push_back(dependency);
    }

    std::vector<runtime::ExpressionHostDependency> host_values;
};

void merge_object(std::map<std::string, runtime::Value, std::less<>>& target,
                  const runtime::Value& value) {
    if (value.object() == nullptr)
        return;
    for (const auto& [name, property] : value.object()->fields) {
        if (name != "$bases")
            target.insert_or_assign(name, property);
    }
}

[[nodiscard]] runtime::Value map_value(std::map<std::string, runtime::Value, std::less<>> values) {
    std::vector<std::pair<std::string, runtime::Value>> fields;
    fields.reserve(values.size());
    for (auto& [name, value] : values)
        fields.emplace_back(std::move(name), std::move(value));
    return runtime::Value(std::move(fields));
}

void set_layout_field(DescriptionNode::Properties& properties, std::string name,
                      runtime::Value value) {
    std::map<std::string, runtime::Value, std::less<>> fields;
    if (const auto current = properties.find("$layout");
        current != properties.end() && current->second.value() != nullptr &&
        current->second.value()->object() != nullptr) {
        for (const auto& [field_name, field_value] : current->second.value()->object()->fields) {
            fields.insert_or_assign(field_name, field_value);
        }
    }
    fields.insert_or_assign(std::move(name), std::move(value));
    properties.insert_or_assign("$layout", ExpressionValue(map_value(std::move(fields))));
}

/**
 * The name of a slot a component declares itself, or none. A component's output also holds the
 * slots of the components it calls; only those described from its own body (its source path)
 * are its own, as the compiler counts them. One it forwards into a child's fill stays its own.
 */
[[nodiscard]] const std::string* own_slot_name(const DescriptionNode& node,
                                               const std::string_view component_path) {
    if (node.type != "Slot" || !node.source_path.starts_with(component_path) ||
        node.source_path.size() <= component_path.size() ||
        node.source_path[component_path.size()] != '/') {
        return nullptr;
    }
    const auto property = node.properties.find("name");
    const runtime::Value* value =
        property != node.properties.end() ? property->second.value() : nullptr;
    return value != nullptr && value->string() != nullptr && !value->string()->empty()
               ? value->string()
               : nullptr;
}

/**
 * Whether a node's children are described with it: a generated provider's rows are described
 * when they are realized, and a walk over a body's output never realizes them.
 */
[[nodiscard]] bool eager_children(const DescriptionNode& node) {
    return dynamic_cast<const EagerDescriptionChildren*>(node.children.get()) != nullptr;
}

void collect_slot_names(const std::shared_ptr<const DescriptionNode>& node,
                        const std::string_view component_path,
                        std::set<std::string, std::less<>>& names) {
    if (const std::string* name = own_slot_name(*node, component_path))
        names.insert(*name);
    if (!eager_children(*node))
        return;
    for (std::size_t index = 0U; index < node->children->size(); ++index) {
        collect_slot_names(node->children->at(index), component_path, names);
    }
}

[[nodiscard]] std::shared_ptr<const DescriptionNode>
project_slots(const std::shared_ptr<const DescriptionNode>& node,
              const std::string_view component_path,
              const std::map<std::string, std::vector<std::shared_ptr<const DescriptionNode>>,
                             std::less<>>& projected) {
    if (const std::string* name = own_slot_name(*node, component_path)) {
        const auto replacement = projected.find(*name);
        if (replacement != projected.end()) {
            auto resolved = std::make_shared<DescriptionNode>(*node);
            resolved->children =
                std::make_shared<const EagerDescriptionChildren>(replacement->second);
            return resolved;
        }
    }

    if (!eager_children(*node))
        return node;
    bool changed = false;
    std::vector<std::shared_ptr<const DescriptionNode>> children;
    children.reserve(node->children->size());
    for (std::size_t index = 0U; index < node->children->size(); ++index) {
        const std::shared_ptr<const DescriptionNode> source = node->children->at(index);
        std::shared_ptr<const DescriptionNode> resolved =
            project_slots(source, component_path, projected);
        changed = changed || resolved != source;
        children.push_back(std::move(resolved));
    }
    if (!changed)
        return node;
    auto resolved = std::make_shared<DescriptionNode>(*node);
    resolved->children = std::make_shared<const EagerDescriptionChildren>(std::move(children));
    return resolved;
}

} // namespace

namespace description_detail {

std::string path_segment(const std::string_view identity) {
    std::string escaped;
    escaped.reserve(identity.size());
    for (const char character : identity) {
        switch (character) {
        case '%':
            escaped += "%25";
            break;
        case '/':
            escaped += "%2F";
            break;
        case ':':
            escaped += "%3A";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

runtime::ScopeFrame item_frame(const Symbol item, const runtime::Value& value,
                               const std::optional<Symbol> index, const std::size_t position) {
    runtime::ScopeFrame frame;
    frame.bindings.push_back(runtime::ScopeBinding{item, true, runtime::ScopeStateBinding::inherit,
                                                   ExpressionValue(value), nullptr});
    if (index.has_value()) {
        frame.bindings.push_back(runtime::ScopeBinding{
            *index, true, runtime::ScopeStateBinding::inherit,
            ExpressionValue(runtime::Value(static_cast<double>(position))), nullptr});
    }
    return frame;
}

void declared_slot_names(const std::shared_ptr<const DescriptionNode>& root,
                         const std::string_view component_path,
                         std::set<std::string, std::less<>>& names) {
    collect_slot_names(root, component_path, names);
}

std::shared_ptr<const DescriptionNode> project_slot_content(
    const std::shared_ptr<const DescriptionNode>& root, const std::string_view component_path,
    const std::map<std::string, std::vector<std::shared_ptr<const DescriptionNode>>, std::less<>>&
        projected) {
    return project_slots(root, component_path, projected);
}

} // namespace description_detail

const std::string& DescriptionBuilder::Scope::instance_path() const noexcept {
    static const std::string none;
    return instance != nullptr ? *instance : none;
}

void DescriptionBuilder::Scope::set_instance(std::string path) {
    instance = std::make_shared<const std::string>(std::move(path));
    expressions.set_component_path(instance);
    loop.reset();
}

void DescriptionBuilder::Scope::enter_loop_item(const std::string_view item,
                                                const std::string_view identity,
                                                std::shared_ptr<KeyedInstances> keyed) {
    const std::size_t parent_length = instance_path().size();
    set_instance(instance_path() + "/" + std::string(item) + ":" + std::string(identity));
    loop = LoopItem{parent_length, std::move(keyed)};
}

std::string DescriptionBuilder::keyed_instance_path(const Scope& scope, const std::string_view type,
                                                    const std::optional<std::string>& key,
                                                    const std::string_view source_path) {
    // An explicit key names the instance among the items of the loop it repeats in, so it takes
    // the place of the item's own segment there: the instance keeps its key, and its state,
    // wherever the item moves. A key the loop's items share (a constant) cannot name one of them,
    // and every later use stays inside its item.
    if (key.has_value() && scope.loop.has_value()) {
        std::string candidate = component_instance_path(
            std::string_view(scope.instance_path()).substr(0U, scope.loop->parent_length), type,
            key, source_path);
        if (scope.loop->keyed->insert(candidate).second)
            return candidate;
    }
    return component_instance_path(scope.instance_path(), type, key, source_path);
}

void DescriptionBuilder::FrameObserver::lexical(const Symbol name, const ExpressionValue& value) {
    // A row's inputs are the values of its enclosing scope it read: a read of a name the row
    // binds itself (a state, a derived value, a nested loop's item) reads no input.
    Frame* const current = builder_.frame();
    if (current == nullptr || current->outer == nullptr || current->outer_reads.contains(name))
        return;
    const ExpressionValue* const outer = current->outer->find(name);
    if (outer != nullptr && runtime::same_expression_value(*outer, value))
        current->outer_reads.emplace(name, runtime::capture_expression_dependency(value));
}

void DescriptionBuilder::FrameObserver::host(const runtime::ExpressionHostDependency& dependency) {
    builder_.observe_host_dependency(dependency);
}

DescriptionBuilder::DescriptionBuilder(runtime::ApplicationContext& application)
    : application_(application), self_(std::make_shared<DescriptionBuilder*>(this)),
      owned_widgets_(std::make_unique<WidgetRegistry>()), widgets_(*owned_widgets_),
      expressions_(std::make_unique<runtime::ExpressionRuntime>(
          application.host(), application.bundle()->action_registry())),
      frame_observer_(*this) {
    static_cast<void>(expressions_->exchange_dependency_observer(&frame_observer_));
    observe_state_changes();
}

DescriptionBuilder::DescriptionBuilder(runtime::ApplicationContext& application,
                                       const WidgetRegistry& widgets)
    : application_(application), self_(std::make_shared<DescriptionBuilder*>(this)),
      owned_widgets_(), widgets_(widgets),
      expressions_(std::make_unique<runtime::ExpressionRuntime>(
          application.host(), application.bundle()->action_registry())),
      frame_observer_(*this) {
    static_cast<void>(expressions_->exchange_dependency_observer(&frame_observer_));
    observe_state_changes();
}

DescriptionBuilder::~DescriptionBuilder() {
    // Rows realized from now on are described by a builder of their own.
    *self_ = nullptr;
    application_.state().stop_observing_changes(state_observer_);
}

void DescriptionBuilder::observe_state_changes() {
    // A write is noted only when a body read the address; a build checks those values alone.
    state_observer_ =
        application_.state().observe_changes([this](const runtime::StateAddress* const address) {
            if (address == nullptr)
                all_states_changed_ = true;
            else if (state_readers_.contains(*address))
                changed_states_.insert(*address);
        });
}

DescriptionBuildResult DescriptionBuilder::build(const runtime::LayerRole role,
                                                 const std::string_view name) {
    const LayerDescriptionRequest request{role, std::string(name)};
    DescriptionLayersBuildResult result = build_layers(std::span(&request, 1U));
    return DescriptionBuildResult{
        std::move(result.roots.front()), std::move(result.diagnostics),
        result.evaluated_expressions,    result.described_nodes,
        result.described_instances,
    };
}

void DescriptionBuilder::set_retained_tree(const RetainedTree* const tree) {
    retained_snapshot_ = tree != nullptr ? tree->description_snapshot() : nullptr;
}

void DescriptionBuilder::set_contextual_host_roots(
    std::map<std::string, runtime::Value, std::less<>> roots) {
    // Unchanged roots keep their identity, which is what instances compare first.
    if (contextual_host_roots_ != nullptr && *contextual_host_roots_ == roots)
        return;
    if (contextual_host_roots_ == nullptr && roots.empty())
        return;
    contextual_host_roots_ = std::make_shared<const runtime::HostRoots>(std::move(roots));
    contextual_host_roots_changed_ = true;
}

void DescriptionBuilder::use_unit(const std::shared_ptr<const runtime::RuntimeUnit>& unit) {
    if (unit_ == unit)
        return;
    reset();
    styles_.clear();
    epoch_ = 0U;
    expressions_->clear_caches();
    unit_ = unit;
    call_widgets_.assign(unit_ != nullptr ? program().call_count() : 0U, std::nullopt);
    call_widgets_revision_ = widgets_.revision();
}

std::shared_ptr<const WidgetRegistry> DescriptionBuilder::widget_snapshot() {
    if (widget_snapshot_ == nullptr || widget_snapshot_revision_ != widgets_.revision()) {
        widget_snapshot_ = std::make_shared<const WidgetRegistry>(widgets_);
        widget_snapshot_revision_ = widgets_.revision();
    }
    return widget_snapshot_;
}

const WidgetLifecycle* DescriptionBuilder::call_widget(const runtime::CallId call) {
    if (call_widgets_revision_ != widgets_.revision()) {
        call_widgets_.assign(call_widgets_.size(), std::nullopt);
        call_widgets_revision_ = widgets_.revision();
    }
    std::optional<const WidgetLifecycle*>& widget = call_widgets_[call];
    if (!widget.has_value())
        widget = widgets_.find(program().call(call).type);
    return *widget;
}

std::vector<std::shared_ptr<const DescriptionNode>>
DescriptionBuilder::build_block(const runtime::BlockId block, const Scope& enclosing,
                                const std::span<const std::size_t> skipped_statement_indices) {
    // The enclosing scope serves until a state or derived declaration extends it; only then is
    // it copied, once for the block.
    std::optional<Scope> extended;
    const Scope* current = &enclosing;
    const auto extend = [&extended, &current, &enclosing]() -> Scope& {
        if (!extended.has_value()) {
            extended.emplace(enclosing);
            current = &*extended;
        }
        return *extended;
    };
    const runtime::Program& program = this->program();
    std::vector<std::shared_ptr<const DescriptionNode>> nodes;
    const std::span<const runtime::ProgramStatement> statements =
        program.statements(program.block(block));
    for (std::size_t statement_index = 0U; statement_index < statements.size(); ++statement_index) {
        if (std::ranges::binary_search(skipped_statement_indices, statement_index))
            continue;
        const runtime::ProgramStatement& statement = statements[statement_index];
        const Scope& scope = *current;
        switch (statement.kind) {
        case runtime::StatementKind::state: {
            Scope& declaring = extend();
            if (statement.state_declaration == runtime::no_program_id)
                throw std::logic_error("indexed state declaration is missing");
            const runtime::UnitStateDeclaration& declaration =
                unit_->state_declarations()[statement.state_declaration];
            const std::string name(statement.name.name());
            const std::string address_scope = "dsl:" + unit_->source_id() + ":" +
                                              declaring.instance_path() +
                                              "/state:" + declaration.declaration_path;
            own_state_scope(address_scope);
            auto binding = std::make_shared<const runtime::LexicalStateBinding>(
                runtime::LexicalStateBinding{runtime::StateAddress{address_scope, name},
                                             std::string(declaring.declaration_scope)});
            const std::shared_ptr<const runtime::ScopeFrame> outer = declaring.expressions.frame();
            // The initializer sees the state's binding, not yet its value.
            runtime::ScopeFrame bound;
            bound.bindings.push_back(runtime::ScopeBinding{
                statement.name, false, runtime::ScopeStateBinding::bound, {}, binding});
            declaring.expressions.push(std::move(bound));
            bind_state_scope(declaring.instance_path(), name, declaring.declaration_scope,
                             address_scope);
            const ExpressionValue evaluated =
                statement.expression != runtime::no_program_id
                    ? evaluate(statement.expression, declaring.expressions)
                    : ExpressionValue{};
            const runtime::Value initial = require_value(evaluated, statement.source);
            // The declaration's type, never this build's initial value: a state keeps one slot
            // type whatever its initializer reads.
            const runtime::StateSlot slot{
                name,
                declaration.type_id,
                initial,
                std::string(declaring.declaration_scope),
            };
            const runtime::StateAddress address{address_scope, name};
            // Describing stores nothing: a state the store does not hold yet starts from its
            // durable value or its initializer, and is stored when the build commits.
            const runtime::Value* const stored = application_.state().peek(address, slot);
            std::optional<runtime::Value> persisted_value;
            if (stored == nullptr && declaration.persistence_key.has_value()) {
                if (const runtime::Value* persisted =
                        application_.durability().application_value(*declaration.persistence_key);
                    persisted != nullptr) {
                    if (declaration.schema != nullptr && declaration.schema->accepts(*persisted)) {
                        persisted_value = *persisted;
                    } else if (transaction_ != nullptr) {
                        transaction_->reports.push_back(runtime::RuntimeDiagnostic{
                            "STRATA.DURABILITY.TYPE_MISMATCH",
                            "Persisted value '" + *declaration.persistence_key +
                                "' no longer matches state '" + name + "' and was discarded.",
                            declaring.instance_path(),
                            slot.type_id,
                            runtime::DiagnosticSeverity::warning,
                            std::nullopt,
                        });
                        transaction_->erased_application_values.push_back(
                            *declaration.persistence_key);
                    }
                }
            }
            const runtime::Value state =
                stored != nullptr ? *stored : persisted_value.value_or(slot.initial_value);
            if (transaction_ != nullptr) {
                transaction_->states.push_back(
                    StateInitialization{address, slot, std::move(persisted_value)});
            }
            observe_state_value(address, state);
            declaring.expressions.set_frame(outer);
            runtime::ScopeFrame declared;
            declared.bindings.push_back(
                runtime::ScopeBinding{statement.name, true, runtime::ScopeStateBinding::bound,
                                      ExpressionValue(state), std::move(binding)});
            declaring.expressions.push(std::move(declared));
            continue;
        }
        case runtime::StatementKind::derived: {
            Scope& declaring = extend();
            const ExpressionValue value = evaluate(statement.expression, declaring.expressions);
            runtime::ScopeFrame declared;
            declared.bindings.push_back(declared_binding(statement.name, value, nullptr));
            declaring.expressions.push(std::move(declared));
            continue;
        }
        case runtime::StatementKind::node:
            nodes.push_back(build_call(statement.call, scope));
            continue;
        case runtime::StatementKind::conditional: {
            const runtime::Value condition =
                require_value(evaluate(statement.expression, scope.expressions), statement.source);
            const runtime::BlockId selected =
                runtime::truthy(condition) ? statement.block : statement.otherwise;
            if (selected != runtime::no_program_id) {
                auto branch = build_block(selected, scope);
                nodes.insert(nodes.end(), branch.begin(), branch.end());
            }
            continue;
        }
        case runtime::StatementKind::when: {
            const runtime::Value subject =
                require_value(evaluate(statement.expression, scope.expressions), statement.source);
            for (const runtime::ProgramWhenBranch& branch : program.branches(statement)) {
                if (branch.match != runtime::no_program_id) {
                    const runtime::Value candidate =
                        require_value(evaluate(branch.match, scope.expressions),
                                      program.expression(branch.match).source);
                    if (candidate != subject)
                        continue;
                }
                auto selected = build_block(branch.block, scope);
                nodes.insert(nodes.end(), selected.begin(), selected.end());
                break;
            }
            continue;
        }
        case runtime::StatementKind::loop: {
            const ExpressionValue collection = evaluate(statement.expression, scope.expressions);
            const runtime::Value* scalar = collection.value();
            const runtime::ValueList* values = scalar != nullptr ? scalar->list() : nullptr;
            if (values == nullptr && collection.collection() != nullptr)
                values = (*collection.collection())->items.list();
            if (values == nullptr)
                continue;
            const std::optional<Symbol> index_name =
                statement.has_index ? std::optional<Symbol>(statement.index_name) : std::nullopt;
            const std::string_view item_name = statement.name.name();
            std::map<std::string, std::size_t, std::less<>> repeated_segments;
            const auto keyed_instances = std::make_shared<Scope::KeyedInstances>();
            // Emitted order, so filtered-out items leave no gap in an entry stagger.
            std::size_t position = 0U;
            for (std::size_t index = 0U; index < values->values.size(); ++index) {
                Scope item_scope = scope;
                item_scope.expressions.push(
                    item_frame(statement.name, values->values[index], index_name, index));
                if (statement.filter != runtime::no_program_id &&
                    !runtime::truthy(
                        require_value(evaluate(statement.filter, item_scope.expressions),
                                      program.expression(statement.filter).source))) {
                    continue;
                }
                // Items without an identity of their own are told apart by position, and equal
                // identities by occurrence: an escaped identity never ends in ":n" itself.
                std::string segment =
                    path_segment(repeated_item_segment(values->values[index], index));
                const std::size_t occurrence = repeated_segments[segment]++;
                if (occurrence != 0U)
                    segment += ":" + std::to_string(occurrence);
                item_scope.enter_loop_item(item_name, segment, keyed_instances);
                auto iteration = build_block(statement.block, item_scope);
                for (std::size_t item = 0U; item < iteration.size(); ++item) {
                    const bool anchor = item == 0U;
                    const bool staggered = iteration[item]->properties.contains("stagger");
                    if (!anchor && !staggered)
                        continue;
                    iteration[item] = annotate_loop_item(
                        iteration[item],
                        anchor ? std::optional<std::string>(
                                     lazy_item_key(values->values[index], index))
                               : std::nullopt,
                        staggered ? std::optional<double>(static_cast<double>(position))
                                  : std::nullopt);
                }
                if (!iteration.empty())
                    ++position;
                nodes.insert(nodes.end(), iteration.begin(), iteration.end());
            }
            continue;
        }
        }
        throw std::logic_error("validated portable IR contains an unknown statement kind");
    }
    return nodes;
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::annotate_loop_item(const std::shared_ptr<const DescriptionNode>& node,
                                       std::optional<std::string> materialization_key,
                                       const std::optional<double> loop_position) {
    // A child instance's output is annotated where the instance places it, so a change of the
    // instance alone is placed again with the same annotations instead of describing this body.
    if (Frame* const current = frame(); current != nullptr) {
        if (const auto found = current->child_index.find(node.get());
            found != current->child_index.end()) {
            const std::size_t position = found->second;
            if (ComponentInstance* const child = find_instance(current->children[position].id);
                child != nullptr) {
                if (materialization_key.has_value())
                    child->call_site.materialization_key = std::move(materialization_key);
                if (loop_position.has_value())
                    child->call_site.loop_position = loop_position;
                emit(*child);
                current->child_index.erase(found);
                current->children[position].seen = child->emitted;
                current->child_index.insert_or_assign(child->emitted.get(), position);
                return child->emitted;
            }
        }
    }
    auto annotated = std::make_shared<DescriptionNode>(*node);
    if (materialization_key.has_value())
        annotated->materialization_key = std::move(materialization_key);
    if (loop_position.has_value()) {
        annotated->properties.insert_or_assign("$loopPosition",
                                               ExpressionValue(runtime::Value(*loop_position)));
    }
    return annotated;
}

std::string DescriptionBuilder::evaluate_repeater_identity(const runtime::IdentityId identity,
                                                           const Scope& scope) {
    const runtime::Program& program = this->program();
    const runtime::ProgramIdentity& node = program.identity(identity);
    switch (node.kind) {
    case runtime::IdentityKind::key: {
        const ExpressionValue value = evaluate(node.expression, scope.expressions);
        const std::optional<std::string> key = key_from_value(value);
        if (!key.has_value() || key->empty()) {
            throw std::invalid_argument(
                "Repeater root key must resolve to a non-empty string, key, or finite number");
        }
        return *key;
    }
    case runtime::IdentityKind::block: {
        std::optional<std::string> result;
        for (const runtime::IdentityId child : program.children(node)) {
            const std::string candidate = evaluate_repeater_identity(child, scope);
            if (candidate.empty())
                continue;
            if (result.has_value())
                throw std::logic_error("validated Repeater identity produced multiple root keys");
            result = candidate;
        }
        if (!result.has_value())
            throw std::logic_error("validated Repeater identity did not select a root key");
        return *result;
    }
    case runtime::IdentityKind::conditional: {
        const runtime::Value selected = require_value(evaluate(node.expression, scope.expressions),
                                                      program.expression(node.expression).source);
        return evaluate_repeater_identity(
            runtime::truthy(selected) ? node.then_identity : node.else_identity, scope);
    }
    case runtime::IdentityKind::when: {
        const runtime::Value subject = require_value(evaluate(node.expression, scope.expressions),
                                                     program.expression(node.expression).source);
        for (const runtime::ProgramIdentityBranch& branch : program.branches(node)) {
            if (branch.match != runtime::no_program_id) {
                const runtime::Value candidate =
                    require_value(evaluate(branch.match, scope.expressions),
                                  program.expression(branch.match).source);
                if (candidate != subject)
                    continue;
            }
            return evaluate_repeater_identity(branch.identity, scope);
        }
        throw std::logic_error("validated Repeater identity when did not select a branch");
    }
    }
    throw std::logic_error("validated Repeater identity contains an unknown extractor kind");
}

const RetainedDescriptionSnapshot::Node*
DescriptionBuilder::retained_widget(const RetainedQuery& query) const noexcept {
    if (retained_snapshot_ == nullptr)
        return nullptr;
    if (query.key.has_value()) {
        return retained_snapshot_->find_key(*query.key, query.source_path, query.state_scope,
                                            query.type);
    }
    const std::vector<const RetainedDescriptionSnapshot::Node*>* candidates =
        retained_snapshot_->find_source(query.source_path);
    if (candidates == nullptr)
        return nullptr;
    const auto found = std::ranges::find_if(*candidates, [&](const auto* candidate) {
        const bool compatible_type = candidate->type == query.type ||
                                     (query.type == "Repeater" && candidate->type == "VirtualList");
        return compatible_type && candidate->state_scope == query.state_scope;
    });
    return found != candidates->end() ? *found : nullptr;
}

DescriptionBuilder::RepeaterChildren DescriptionBuilder::build_repeater_children(
    const runtime::BlockId block, const Scope& scope, ComponentInstance& generator,
    const RetainedDescriptionSnapshot::Node* const retained_widget) {
    const runtime::Program& program = this->program();
    const std::span<const runtime::ProgramStatement> statements =
        program.statements(program.block(block));
    if (statements.size() != 1U || statements.front().kind != runtime::StatementKind::loop)
        return {};
    const runtime::ProgramStatement& statement = statements.front();
    const Symbol item_name = statement.name;
    const std::optional<Symbol> index_name =
        statement.has_index ? std::optional<Symbol>(statement.index_name) : std::nullopt;
    if (statement.identity == runtime::no_program_id)
        throw std::logic_error("validated Repeater loop lost its root-key extractor");

    DescriptionSequenceGeneration stamp;
    stamp.active_unit = application_.active_generation().value_or(0U);
    std::shared_ptr<const runtime::IndexableSequence> sequence;
    // The generator keeps its last generation; a new one takes the retained tree's, if any.
    const bool kept = generator.sequence != nullptr && generator.sequence_generation.has_value();
    const std::shared_ptr<const runtime::IndexableSequence> previous_sequence =
        kept                         ? generator.sequence
        : retained_widget != nullptr ? retained_widget->virtual_sequence.lock()
                                     : nullptr;
    const DescriptionSequenceGeneration* const previous_stamp =
        kept ? &*generator.sequence_generation
        : retained_widget != nullptr && retained_widget->virtual_sequence_generation.has_value()
            ? &*retained_widget->virtual_sequence_generation
            : nullptr;
    {
        // Source reads belong to the retained generation just as filter/identity reads do. The
        // loop domain is excluded only after source evaluation so an outer binding with the same
        // name is still traced when it participates in the source expression.
        RepeaterExpressionDependencies dependencies(expressions_->dependency_observer());
        ExpressionDependencyObserverRestore dependency_observer(*expressions_, &dependencies);
        const ExpressionValue collection = evaluate(statement.expression, scope.expressions);
        const runtime::Value* scalar = collection.value();
        const runtime::ValueList* values = scalar != nullptr ? scalar->list() : nullptr;
        if (values == nullptr && collection.collection() != nullptr)
            values = (*collection.collection())->items.list();
        if (values == nullptr)
            return {};
        const runtime::Value source_items =
            scalar != nullptr ? *scalar : (*collection.collection())->items;
        stamp.source = runtime::capture_expression_dependency(collection);

        if (previous_sequence != nullptr && previous_stamp != nullptr &&
            previous_stamp->active_unit == stamp.active_unit &&
            previous_stamp->source == stamp.source &&
            repeater_dependencies_current(*previous_stamp, scope.expressions, *expressions_)) {
            stamp = *previous_stamp;
            sequence = previous_sequence;
            // The kept generation still rests on what its filter and keys read, and whoever
            // holds this Repeater reads those values with it.
            if (dependencies.upstream != nullptr) {
                for (const auto& [name, value] : stamp.lexical_dependencies) {
                    static_cast<void>(value);
                    if (const ExpressionValue* current = scope.expressions.find(name))
                        dependencies.upstream->lexical(name, *current);
                }
                for (const auto& [canonical, dependency] : stamp.host_dependencies) {
                    static_cast<void>(canonical);
                    dependencies.upstream->host(dependency);
                }
            }
        } else {
            dependencies.exclude(item_name);
            if (index_name.has_value())
                dependencies.exclude(*index_name);
            std::optional<std::vector<std::size_t>> selection;
            if (statement.filter != runtime::no_program_id) {
                selection.emplace();
                selection->reserve(values->values.size());
            }
            std::vector<std::string> keys;
            RepeaterIndexableSequence::KeyIndexes indexes;
            keys.reserve(values->values.size());
            indexes.reserve(values->values.size());
            const auto source_index_of = [&selection](const std::size_t visible) {
                return selection.has_value() ? (*selection)[visible] : visible;
            };
            for (std::size_t index = 0U; index < values->values.size(); ++index) {
                Scope item_scope = scope;
                item_scope.expressions.push(
                    item_frame(item_name, values->values[index], index_name, index));
                if (statement.filter != runtime::no_program_id &&
                    !runtime::truthy(
                        require_value(evaluate(statement.filter, item_scope.expressions),
                                      program.expression(statement.filter).source))) {
                    continue;
                }
                std::string canonical_key =
                    evaluate_repeater_identity(statement.identity, item_scope);
                const auto [duplicate, inserted] = indexes.try_emplace(canonical_key, keys.size());
                if (!inserted) {
                    throw std::invalid_argument("Repeater root key '" + canonical_key +
                                                "' is duplicated at source indexes " +
                                                std::to_string(source_index_of(duplicate->second)) +
                                                " and " + std::to_string(index));
                }
                keys.push_back(std::move(canonical_key));
                if (selection.has_value())
                    selection->push_back(index);
            }
            stamp.lexical_dependencies = std::move(dependencies.lexical_values);
            stamp.host_dependencies = std::move(dependencies.host_values);
            const std::uint64_t previous_generation =
                previous_sequence != nullptr ? previous_sequence->generation() : 0U;
            if (previous_generation == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("repeater sequence generation exhausted");
            sequence = std::make_shared<const RepeaterIndexableSequence>(
                previous_generation + 1U, source_items, std::move(selection), std::move(keys),
                std::move(indexes));
        }
    }

    if (sequence != generator.sequence) {
        // Rows whose keys the new generation lacks are never asked for by its provider.
        for (auto row = generator.rows.begin(); row != generator.rows.end();) {
            if (sequence->index_of_key(row->first).has_value()) {
                ++row;
                continue;
            }
            const InstanceId id = row->second;
            row = generator.rows.erase(row);
            if (ComponentInstance* const stale = find_instance(id); stale != nullptr)
                forget(*stale);
        }
    }
    generator.sequence = sequence;
    generator.sequence_generation = stamp;
    auto rows = std::make_shared<RowSource>();
    rows->kind = RowSource::Kind::repeater;
    rows->scope = scope;
    rows->count = sequence->count();
    rows->sequence = sequence;
    rows->item = item_name;
    rows->index = index_name;
    rows->block = statement.block;
    std::shared_ptr<const DescriptionChildren> provider = provide(generator, std::move(rows));
    return RepeaterChildren{std::move(provider), std::move(sequence), std::move(stamp)};
}

std::shared_ptr<const DescriptionNode> DescriptionBuilder::build_call(const runtime::CallId id,
                                                                      const Scope& scope) {
    const runtime::ProgramCall& call = program().call(id);
    if (call.component)
        return build_component_call(call, scope);
    DescriptionNode::Properties properties;
    // Room for the arguments and the few properties description adds ($layout, defaults).
    properties.reserve(call.arguments_count + 4U);
    for (const runtime::ProgramCallArgument& argument : program().arguments(call)) {
        properties.emplace(argument.name.name(), evaluate(argument.value, scope.expressions));
    }
    std::vector<DescriptionBehavior> behaviors;
    if (call.has_behaviors)
        behaviors = build_behaviors(call, scope.expressions);
    std::string type = call.type;
    const std::string& source_path = call.path;
    if (scope.widget_defaults != nullptr) {
        if (const auto defaults = scope.widget_defaults->find(type);
            defaults != scope.widget_defaults->end()) {
            if (!properties.contains("style") && defaults->second.style.has_value())
                properties.emplace("style", *defaults->second.style);
            if (!properties.contains("variant") && defaults->second.variant.has_value())
                properties.emplace("variant", *defaults->second.variant);
        }
    }
    normalize_layout(properties);
    const auto key_property = properties.find("key");
    std::optional<std::string> key =
        key_property != properties.end() ? key_from_value(key_property->second) : std::nullopt;
    const WidgetLifecycle* const widget = call_widget(id);
    if (!key.has_value() && widget != nullptr && !widget->describe.implicit_key_prefix.empty())
        key = widget->describe.implicit_key_prefix + source_path;
    const std::string retained_type = widget != nullptr && !widget->describe.canonical_type.empty()
                                          ? widget->describe.canonical_type
                                          : type;
    const RetainedQuery retained_query{
        key,
        source_path,
        scope.instance_path(),
        retained_type,
    };
    const RetainedDescriptionSnapshot::Node* retained = retained_widget(retained_query);
    RetainedDescriptionSnapshot::Node durable_retained;
    if (retained == nullptr) {
        const auto persistence_property = properties.find("persistenceKey");
        const runtime::Value* persistence_value = persistence_property != properties.end()
                                                      ? persistence_property->second.value()
                                                      : nullptr;
        const std::string* persistence_key =
            persistence_value != nullptr ? persistence_value->string() : nullptr;
        if (persistence_key != nullptr && !persistence_key->empty() && widget != nullptr) {
            for (const std::string& field : widget->persistence.retained_fields) {
                if (const runtime::Value* value =
                        application_.durability().widget_value(*persistence_key, field);
                    value != nullptr) {
                    if (widget->persistence.accepts == nullptr ||
                        widget->persistence.accepts(field, *value)) {
                        durable_retained.retained_values.emplace(field, *value);
                    } else {
                        application_.services().report(runtime::RuntimeDiagnostic{
                            "STRATA.DURABILITY.TYPE_MISMATCH",
                            "Persisted widget field '" + field + "' for '" + *persistence_key +
                                "' has an invalid value and was discarded.",
                            scope.instance_path(),
                            type,
                            runtime::DiagnosticSeverity::warning,
                            std::nullopt,
                        });
                        static_cast<void>(
                            application_.durability().erase_widget_value(*persistence_key, field));
                    }
                }
            }
            if (!durable_retained.retained_values.empty()) {
                durable_retained.type = type;
                durable_retained.key = key;
                durable_retained.source_path = source_path;
                durable_retained.state_scope = scope.instance_path();
                retained = &durable_retained;
            }
        }
    }
    // A call providing generated rows is an instance of its own: its rows are its children.
    ComponentInstance* rows_generator = nullptr;
    RepeaterChildren repeater_children;
    if (type == "Repeater" && call.children != runtime::no_program_id) {
        const std::span<const runtime::ProgramStatement> statements =
            program().statements(program().block(call.children));
        if (statements.size() == 1U && statements.front().kind == runtime::StatementKind::loop) {
            rows_generator = &generator(scope, call, key);
            repeater_children =
                build_repeater_children(call.children, scope, *rows_generator, retained);
        }
    }
    widgets_.apply_layout_defaults(type, properties);

    std::vector<std::shared_ptr<const DescriptionNode>> child_nodes;
    if (call.children != runtime::no_program_id && repeater_children.source == nullptr)
        child_nodes = build_block(call.children, scope);
    WidgetDescriptionExpansion expansion = widgets_.expand_description(
        WidgetDescriptionExpansion{
            .type = std::move(type),
            .key = std::move(key),
            .properties = std::move(properties),
            .children = std::move(child_nodes),
            .behaviors = std::move(behaviors),
            .synthesized_nodes = 0U,
            .generated_children = repeater_children.source,
            .generated_widget_children = nullptr,
        },
        scope.instance_path(), application_.bundle()->action_registry(), retained,
        // Called only while the widget expands, within this call.
        [this, &scope](const std::string_view component, std::string template_key,
                       WidgetTemplateArguments arguments) {
            return build_component_template(component, std::move(template_key),
                                            std::move(arguments), scope);
        },
        WidgetRetainedDependencyObserver{
            [this, retained_query](const std::string_view name, const runtime::Value* const value) {
                observe_retained_value(retained_query, name, value);
            }});
    std::shared_ptr<const DescriptionChildren> generated_children =
        std::move(expansion.generated_children);
    WidgetGeneratedVirtualization generated_virtualization;
    if (expansion.generated_widget_children != nullptr) {
        if (generated_children != nullptr)
            throw std::logic_error("widget description installed two generated child providers");
        const std::shared_ptr<const WidgetGeneratedChildren> generated =
            std::move(expansion.generated_widget_children);
        generated_virtualization = generated->virtualization;
        rows_generator = &generator(scope, call, retained_query.key);
        auto rows = std::make_shared<RowSource>();
        rows->kind = RowSource::Kind::widget;
        rows->scope = scope;
        rows->count = generated->count;
        rows->factory = generated->factory;
        generated_children = provide(*rows_generator, std::move(rows));
    }
    type = std::move(expansion.type);
    key = std::move(expansion.key);
    properties = std::move(expansion.properties);
    child_nodes = std::move(expansion.children);
    behaviors = std::move(expansion.behaviors);
    if (repeater_children.source != nullptr)
        set_layout_field(properties, "virtualMeasureItemExtents", runtime::Value(true));
    described_nodes_ += expansion.synthesized_nodes;
    ++described_nodes_;
    const auto content_key_property = properties.find("contentKey");
    const auto content_transition_property = properties.find("contentTransition");
    if (content_key_property != properties.end() &&
        content_transition_property != properties.end() &&
        content_transition_property->second.value() != nullptr &&
        content_transition_property->second.value()->string() != nullptr) {
        const std::optional<std::string> content_key = key_from_value(content_key_property->second);
        if (content_key.has_value()) {
            const runtime::Value* source_layout = [&properties]() -> const runtime::Value* {
                const auto found = properties.find("$layout");
                return found != properties.end() ? found->second.value() : nullptr;
            }();
            std::vector<std::pair<std::string, runtime::Value>> item_layout_fields;
            const auto copy_layout_field = [&source_layout,
                                            &item_layout_fields](const std::string_view name) {
                const runtime::Value* value =
                    source_layout != nullptr ? source_layout->field(name) : nullptr;
                if (value != nullptr)
                    item_layout_fields.emplace_back(std::string(name), *value);
            };
            copy_layout_field("kind");
            copy_layout_field("gap");
            copy_layout_field("alignItems");
            copy_layout_field("justifyContent");
            copy_layout_field("alignContent");
            copy_layout_field("wrap");
            item_layout_fields.emplace_back(
                "width", runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                             {"weight", runtime::Value(1.0)},
                         }));
            // A definite container height (fixed, fill or fraction) belongs to layout: the
            // coordinator and item fill it so fill descendants resolve against it, and no
            // content-size motion runs on that axis. Only a content-sized container follows the
            // incoming item's measured height.
            const runtime::Value* container_height =
                source_layout != nullptr ? source_layout->field("height") : nullptr;
            const bool definite_height =
                container_height != nullptr &&
                (container_height->number() != nullptr || container_height->object() != nullptr);
            const auto fill_height = [] {
                return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                    {"weight", runtime::Value(1.0)},
                });
            };
            item_layout_fields.emplace_back("height", definite_height ? fill_height()
                                                                      : runtime::Value("content"));
            DescriptionNode::Properties item_properties;
            item_properties.emplace("$layout",
                                    ExpressionValue(runtime::Value(std::move(item_layout_fields))));
            item_properties.emplace("transition",
                                    ExpressionValue(*content_transition_property->second.value()));
            const auto content_transition_mode = properties.find("contentTransitionMode");
            const runtime::Value* transition_mode = content_transition_mode != properties.end()
                                                        ? content_transition_mode->second.value()
                                                        : nullptr;
            item_properties.emplace(
                "$transitionSequence",
                ExpressionValue(runtime::Value(transition_mode != nullptr &&
                                                       transition_mode->string() != nullptr
                                                   ? *transition_mode->string()
                                                   : "OUT_IN")));
            auto item = DescriptionNode::create(
                "AnimatedContentItem", *content_key, source_path, scope.instance_path(),
                std::move(item_properties),
                std::make_shared<const EagerDescriptionChildren>(std::move(child_nodes)));
            DescriptionNode::Properties coordinator_properties;
            coordinator_properties.emplace(
                "$layout",
                ExpressionValue(runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                    {"clip", runtime::Value(true)},
                    {"height", definite_height ? fill_height() : runtime::Value("content")},
                    {"kind", runtime::Value("STACK")},
                    {"width", runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                                  {"weight", runtime::Value(1.0)},
                              })},
                })));
            coordinator_properties.emplace(
                "animateContentSize",
                ExpressionValue(runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                    {"clip", runtime::Value(true)},
                    {"height", runtime::Value(!definite_height)},
                    {"width", runtime::Value(false)},
                })));
            auto coordinator = DescriptionNode::create(
                "AnimatedContent", "strata.content." + key.value_or(source_path), source_path,
                scope.instance_path(), std::move(coordinator_properties),
                std::make_shared<const EagerDescriptionChildren>(
                    std::vector<std::shared_ptr<const DescriptionNode>>{std::move(item)}));
            child_nodes = {std::move(coordinator)};
            described_nodes_ += 2U;
        }
    }
    // Content-replacement arguments are description-expansion controls. The retained parent does
    // not render or lay out from them after the AnimatedContent coordinator has been built, so
    // retaining them would falsely invalidate the parent's fragment whenever the content key
    // changes.
    properties.erase("contentKey");
    properties.erase("contentTransition");
    properties.erase("contentTransitionMode");
    std::shared_ptr<const DescriptionNode> result = DescriptionNode::create(
        type, key, source_path, scope.instance_path(), std::move(properties),
        generated_children != nullptr
            ? std::move(generated_children)
            : std::shared_ptr<const DescriptionChildren>(
                  std::make_shared<const EagerDescriptionChildren>(std::move(child_nodes))),
        std::move(behaviors));
    if (generated_virtualization.sequence != nullptr ||
        generated_virtualization.item_members != nullptr ||
        generated_virtualization.item_extents != nullptr || repeater_children.sequence != nullptr) {
        auto virtualized = std::make_shared<DescriptionNode>(*result);
        virtualized->virtual_sequence = generated_virtualization.sequence != nullptr
                                            ? std::move(generated_virtualization.sequence)
                                            : std::move(repeater_children.sequence);
        virtualized->virtual_sequence_generation = std::move(repeater_children.generation);
        virtualized->virtual_item_members = std::move(generated_virtualization.item_members);
        virtualized->virtual_item_extents = std::move(generated_virtualization.item_extents);
        result = std::move(virtualized);
    }
    const WidgetLifecycle* lifecycle = widgets_.find(type);
    if (lifecycle != nullptr && lifecycle->describe.starts_unmaterialized) {
        // A virtual viewport starts unresolved. Its retained collection owner realizes the range
        // published by layout; guessing here cannot account for safe insets, scroll chrome,
        // measured extents, or a retained scroll anchor.
        auto ranged = std::make_shared<DescriptionNode>(*result);
        ranged->materialization = MaterializationRange{};
        result = std::move(ranged);
    }
    if (rows_generator == nullptr)
        return result;
    // The generator's output is this node; a changed row hands out a copy with a new provider.
    ComponentInstance& rows = *rows_generator;
    rows.root = share_unchanged_description(rows.root, std::move(result));
    rows.call_site = CallSite{};
    rows.dirty = false;
    rows.reprovision = false;
    rows.settled_epoch = epoch_;
    rows.last_used_epoch = epoch_;
    emit(rows);
    hold_child(rows);
    return rows.emitted;
}

DescriptionBuilder::Scope DescriptionBuilder::component_scope(
    const runtime::ProgramComponent& component, std::string instance_path, const Scope& caller,
    std::shared_ptr<const Scope::WidgetDefaults> defaults,
    const std::span<const ExpressionValue* const> supplied, ComponentInputs* const inputs) {
    Scope result;
    result.declaration_scope = component.declaration_scope;
    result.widget_defaults = std::move(defaults);
    result.expressions.set_contextual_host_roots(caller.expressions.shared_contextual_host_roots());
    result.set_instance(std::move(instance_path));
    // The body sees only its parameters, but the caller's state bindings still reach it.
    runtime::ScopeFrame frame;
    frame.parent = caller.expressions.frame();
    frame.hides_parent_values = true;
    const std::span<const runtime::ProgramParameter> parameters = program().parameters(component);
    frame.bindings.reserve(parameters.size());
    for (std::size_t index = 0U; index < parameters.size(); ++index) {
        const runtime::ProgramParameter& parameter = parameters[index];
        if (const ExpressionValue* value = supplied[index]) {
            frame.bindings.push_back(declared_binding(
                parameter.name, *value,
                parameter.state_binding ? value->shared_lexical_state_binding() : nullptr));
            if (inputs != nullptr)
                inputs->push_back(*value);
            continue;
        }
        ExpressionValue evaluated;
        if (parameter.default_value != runtime::no_program_id) {
            // A default sees the parameters bound before it.
            runtime::ExpressionScope bound = result.expressions;
            bound.set_frame(std::make_shared<const runtime::ScopeFrame>(frame));
            evaluated = evaluate(parameter.default_value, bound);
        }
        frame.bindings.push_back(declared_binding(parameter.name, evaluated, nullptr));
        if (inputs != nullptr)
            inputs->push_back(std::move(evaluated));
    }
    result.expressions.set_frame(std::make_shared<const runtime::ScopeFrame>(std::move(frame)));
    return result;
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::build_component_call(const runtime::ProgramCall& call, const Scope& scope) {
    const runtime::Program& program = this->program();
    const runtime::ProgramComponent& component = program.component(call.component_index);
    const std::string& type = call.type;
    const std::string& source_path = call.path;
    const std::span<const runtime::ProgramCallArgument> call_arguments = program.arguments(call);
    std::vector<ExpressionValue> arguments;
    arguments.reserve(call_arguments.size());
    for (const runtime::ProgramCallArgument& argument : call_arguments)
        arguments.push_back(evaluate(argument.value, scope.expressions));
    std::vector<DescriptionBehavior> behaviors;
    if (call.has_behaviors)
        behaviors = build_behaviors(call, scope.expressions);
    const std::optional<std::string> key = call.key_argument != runtime::no_program_id
                                               ? key_from_value(arguments[call.key_argument])
                                               : std::nullopt;

    // Caller content is projected in the caller's scope, extended only by the component's
    // widget defaults when it declares any.
    std::optional<Scope> projected;
    if (const std::span<const runtime::ProgramWidgetDefault> declared =
            program.widget_defaults(component);
        !declared.empty()) {
        auto defaults = scope.widget_defaults != nullptr
                            ? std::make_shared<Scope::WidgetDefaults>(*scope.widget_defaults)
                            : std::make_shared<Scope::WidgetDefaults>();
        for (const runtime::ProgramWidgetDefault& entry : declared) {
            Scope::WidgetDefault values;
            if (entry.style != runtime::no_program_id)
                values.style = evaluate(entry.style, scope.expressions);
            if (entry.variant != runtime::no_program_id)
                values.variant = evaluate(entry.variant, scope.expressions);
            defaults->insert_or_assign(entry.widget, std::move(values));
        }
        projected.emplace(scope);
        projected->widget_defaults = std::move(defaults);
    }
    const Scope& projection_scope = projected.has_value() ? *projected : scope;

    std::map<std::string, std::vector<std::shared_ptr<const DescriptionNode>>, std::less<>>
        projected_content;
    std::vector<std::shared_ptr<const DescriptionNode>> raw_content;
    if (call.children != runtime::no_program_id) {
        for (const runtime::ProgramSlotFill& fill : program.slot_fills(call)) {
            if (fill.name == runtime::no_program_id)
                continue;
            const ExpressionValue name_value = evaluate(fill.name, projection_scope.expressions);
            const runtime::Value* scalar = name_value.value();
            if (scalar == nullptr || scalar->string() == nullptr || scalar->string()->empty())
                continue;
            projected_content.insert_or_assign(
                *scalar->string(), fill.children == runtime::no_program_id
                                       ? std::vector<std::shared_ptr<const DescriptionNode>>{}
                                       : build_block(fill.children, projection_scope));
        }
        const std::span<const std::size_t> projected_statements =
            program.projected_statements(call);
        if (program.block(call.children).statements_count > projected_statements.size())
            raw_content = build_block(call.children, projection_scope, projected_statements);
    }

    const std::span<const std::uint32_t> parameter_arguments = program.parameter_arguments(call);
    std::vector<const ExpressionValue*> supplied;
    supplied.reserve(parameter_arguments.size());
    for (const std::uint32_t argument : parameter_arguments)
        supplied.push_back(argument != runtime::no_program_id ? &arguments[argument] : nullptr);
    ComponentInputs inputs;
    inputs.reserve(parameter_arguments.size() + arguments.size());
    Scope instance = component_scope(
        component, keyed_instance_path(scope, type, key, source_path), scope,
        projection_scope.widget_defaults, supplied, &inputs);
    // Arguments no parameter takes, and the widget defaults the body sees, describe it too.
    for (std::uint32_t index = 0U; index < arguments.size(); ++index) {
        if (std::ranges::find(parameter_arguments, index) == parameter_arguments.end())
            inputs.push_back(arguments[index]);
    }
    if (instance.widget_defaults != nullptr) {
        for (const auto& [widget, defaults] : *instance.widget_defaults) {
            inputs.emplace_back(runtime::Value(widget));
            inputs.emplace_back(runtime::Value(defaults.style.has_value()));
            inputs.push_back(defaults.style.value_or(ExpressionValue{}));
            inputs.emplace_back(runtime::Value(defaults.variant.has_value()));
            inputs.push_back(defaults.variant.value_or(ExpressionValue{}));
        }
    }

    ComponentInstance& called =
        this->instance(InstanceKind::component, type + "\n" + instance.instance_path(),
                       frame() != nullptr ? frame()->instance : nullptr);
    // The instance is described again only when it is stale or called with other inputs.
    const bool current =
        called.root != nullptr && !called.dirty &&
        called.component_index == call.component_index &&
        called.source_path == source_path && same_inputs(called.inputs, inputs) &&
        same_contextual_host_roots(called.contextual_host_roots);
    called.scope = std::move(instance);
    if (current) {
        called.last_used_epoch = epoch_;
    } else {
        called.component_index = call.component_index;
        called.source_path = source_path;
        called.inputs = std::move(inputs);
        called.contextual_host_roots = contextual_host_roots_;
        describe_component(called);
    }

    CallSite site;
    site.component_path = component.path;
    if (call.children != runtime::no_program_id) {
        const std::set<std::string, std::less<>>& slots = declared_slots(called);
        if (!raw_content.empty()) {
            const std::string shorthand = slots.size() == 1U       ? *slots.begin()
                                          : slots.contains("content") ? std::string("content")
                                                                      : std::string{};
            if (!shorthand.empty())
                projected_content.insert_or_assign(shorthand, std::move(raw_content));
        }
        for (auto content = projected_content.begin(); content != projected_content.end();) {
            if (!slots.contains(content->first))
                content = projected_content.erase(content);
            else
                ++content;
        }
        site.projected = std::move(projected_content);
    }
    site.behaviors = std::move(behaviors);
    called.call_site = std::move(site);
    emit(called);
    hold_child(called);
    return called.emitted;
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::build_component_template(const std::string_view component_name, std::string key,
                                             WidgetTemplateArguments arguments,
                                             const Scope& caller) {
    const std::optional<std::uint32_t> index = program().component_index(component_name);
    if (!index.has_value()) {
        throw std::logic_error("component template '" + std::string(component_name) +
                               "' lost its declaration");
    }
    const runtime::ProgramComponent& component = program().component(*index);
    std::shared_ptr<const Scope::WidgetDefaults> defaults = caller.widget_defaults;
    if (const std::span<const runtime::ProgramWidgetDefault> declared =
            program().widget_defaults(component);
        !declared.empty()) {
        auto extended = defaults != nullptr ? std::make_shared<Scope::WidgetDefaults>(*defaults)
                                            : std::make_shared<Scope::WidgetDefaults>();
        for (const runtime::ProgramWidgetDefault& entry : declared) {
            Scope::WidgetDefault values;
            if (entry.style != runtime::no_program_id)
                values.style = evaluate(entry.style, caller.expressions);
            if (entry.variant != runtime::no_program_id)
                values.variant = evaluate(entry.variant, caller.expressions);
            extended->insert_or_assign(entry.widget, std::move(values));
        }
        defaults = std::move(extended);
    }
    const std::span<const runtime::ProgramParameter> parameters = program().parameters(component);
    std::vector<const ExpressionValue*> supplied;
    supplied.reserve(parameters.size());
    for (const runtime::ProgramParameter& parameter : parameters) {
        const auto found = arguments.find(parameter.name.name());
        supplied.push_back(found != arguments.end() ? &found->second : nullptr);
    }
    const Scope instance = component_scope(
        component,
        // A template key is the widget's, not an item identity the author chose: the instance
        // stays inside the loop item that called it.
        component_instance_path(caller.instance_path(), component_name, key, "$template:" + key),
        caller, std::move(defaults), supplied, nullptr);
    std::vector<std::shared_ptr<const DescriptionNode>> roots =
        build_block(component.body, instance);
    if (roots.size() != 1U)
        throw std::logic_error("validated component template must describe exactly one root node");
    return std::move(roots.front());
}

std::vector<DescriptionBehavior>
DescriptionBuilder::build_behaviors(const runtime::ProgramCall& call,
                                    const runtime::ExpressionScope& scope) {
    std::vector<DescriptionBehavior> result;
    for (const runtime::ProgramBehavior& entry : program().behaviors(call)) {
        const ExpressionValue id_value = evaluate(entry.id, scope);
        const runtime::Value* id_scalar = id_value.value();
        if (id_scalar == nullptr || id_scalar->string() == nullptr || id_scalar->string()->empty())
            continue;
        DescriptionBehavior behavior;
        behavior.id = *id_scalar->string();
        if (entry.enabled != runtime::no_program_id) {
            const ExpressionValue enabled_value = evaluate(entry.enabled, scope);
            if (enabled_value.value() != nullptr && enabled_value.value()->boolean() != nullptr)
                behavior.enabled = *enabled_value.value()->boolean();
        }
        if (entry.options != runtime::no_program_id) {
            behavior.options = require_value(evaluate(entry.options, scope),
                                             program().expression(entry.options).source);
        } else {
            behavior.options =
                runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{});
        }
        if (entry.action != runtime::no_program_id) {
            const ExpressionValue action_value = evaluate(entry.action, scope);
            if (action_value.action() != nullptr && *action_value.action() != nullptr)
                behavior.action = *action_value.action();
        }
        result.push_back(std::move(behavior));
    }
    return result;
}

void DescriptionBuilder::bind_state_scope(const std::string_view runtime_scope,
                                          const std::string_view state_name,
                                          const std::string_view declaration_scope,
                                          const std::string_view address_scope) {
    Frame* const current = frame();
    if (current == nullptr)
        return;
    current->bindings.insert_or_assign(
        runtime::StateAddress{std::string(runtime_scope), std::string(state_name)},
        StateBindingEffect{std::string(declaration_scope), std::string(address_scope)});
}

void DescriptionBuilder::own_state_scope(const std::string_view scope) {
    if (Frame* const current = frame(); current != nullptr)
        current->owned_scopes.insert(std::string(scope));
}

void DescriptionBuilder::observe_state_value(const runtime::StateAddress& address,
                                             const runtime::Value& value) {
    if (Frame* const current = frame(); current != nullptr)
        current->reads.state.insert_or_assign(address, value);
}

void DescriptionBuilder::observe_host_dependency(
    const runtime::ExpressionHostDependency& dependency) {
    if (Frame* const current = frame(); current != nullptr) {
        current->reads.host.insert_or_assign(
            runtime::canonical_host_dependency_path(dependency.path), dependency);
    }
}

void DescriptionBuilder::observe_retained_value(const RetainedQuery& query,
                                                const std::string_view name,
                                                const runtime::Value* const value) {
    Frame* const current = frame();
    if (current == nullptr)
        return;
    std::optional<runtime::Value> observed =
        value != nullptr ? std::optional(*value) : std::nullopt;
    std::vector<RetainedRead>& reads = current->reads.retained;
    const auto found = std::ranges::find_if(reads, [&](const RetainedRead& read) {
        return read.name == name && read.query == query;
    });
    if (found != reads.end()) {
        found->value = std::move(observed);
        return;
    }
    reads.push_back(RetainedRead{query, std::string(name), std::move(observed)});
}

bool DescriptionBuilder::host_read_current(
    const std::string_view canonical, const runtime::ExpressionHostDependency& dependency) const {
    // A surface's own roots are compared as a whole, by whoever holds them.
    if (dependency.contextual)
        return true;
    const runtime::HostStore& host = application_.host();
    if (host_reads_count_ != host.invalidation_count()) {
        host_reads_.clear();
        host_reads_count_ = host.invalidation_count();
    }
    // Siblings read the same paths: each is resolved once per host generation.
    auto read = host_reads_.find(canonical);
    if (read == host_reads_.end())
        read = host_reads_.emplace(std::string(canonical), HostRead{}).first;
    HostRead& current = read->second;
    // What the component read, not which snapshot generation it came from: a host that
    // republishes often (a HUD every frame) keeps every component whose values stayed.
    if (dependency.value.has_value()) {
        if (!current.value.has_value())
            current.value = host.resolve(dependency.path);
        return current.value->has_value() && **current.value == *dependency.value;
    }
    if (!current.origin.has_value())
        current.origin = host.origin(dependency.path);
    const std::optional<std::pair<std::string, std::uint64_t>>& origin = *current.origin;
    return origin.has_value() == dependency.snapshot_id.has_value() &&
           (!origin.has_value() || (origin->first == *dependency.snapshot_id &&
                                    origin->second == dependency.snapshot_generation));
}

bool DescriptionBuilder::same_contextual_host_roots(
    const std::shared_ptr<const runtime::HostRoots>& roots) const {
    if (roots == contextual_host_roots_)
        return true;
    static const runtime::HostRoots none;
    return (roots != nullptr ? *roots : none) ==
           (contextual_host_roots_ != nullptr ? *contextual_host_roots_ : none);
}

ExpressionValue DescriptionBuilder::evaluate(const runtime::ExpressionId expression,
                                             const runtime::ExpressionScope& scope) {
    ++evaluated_expressions_;
    ExpressionValue value = expressions_->evaluate(program(), expression, scope);
    if (!expressions_->diagnostics().empty())
        append_diagnostics(*expressions_);
    return value;
}

runtime::Value DescriptionBuilder::require_value(const ExpressionValue& value,
                                                 const data::JsonView source) {
    if (value.value() != nullptr)
        return *value.value();
    if (value.collection() != nullptr)
        return (*value.collection())->items;
    const std::optional<std::string_view> expression_path = source.find("path").string();
    if (Frame* const current = frame(); current != nullptr)
        current->reads.raised_errors = true;
    diagnostics_.push_back(runtime::RuntimeDiagnostic{
        "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
        "Description expression did not produce a scalar value.",
        expression_path.has_value() ? std::string(*expression_path) : std::string{},
        std::string("scalar value"),
        runtime::DiagnosticSeverity::error,
        runtime::portable_expression_range(source),
    });
    return runtime::Value{};
}

void DescriptionBuilder::append_diagnostics(runtime::ExpressionRuntime& expressions) {
    // The body being described raised them; the bodies around it did not.
    if (Frame* const current = frame();
        current != nullptr &&
        std::ranges::any_of(expressions.diagnostics(), [](const runtime::RuntimeDiagnostic& item) {
            return item.severity == runtime::DiagnosticSeverity::error;
        })) {
        current->reads.raised_errors = true;
    }
    diagnostics_.insert(diagnostics_.end(), expressions.diagnostics().begin(),
                        expressions.diagnostics().end());
    expressions.clear_diagnostics();
}

runtime::Value DescriptionBuilder::resolve_style(const runtime::Value& value,
                                                 std::set<std::string, std::less<>>& resolving) {
    if (value.string() != nullptr)
        return resolve_named_style(*value.string(), resolving);
    if (value.object() == nullptr)
        return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{});
    std::map<std::string, runtime::Value, std::less<>> merged;
    if (const runtime::Value* bases = value.field("$bases");
        bases != nullptr && bases->list() != nullptr) {
        for (const runtime::Value& base : bases->list()->values) {
            merge_object(merged, resolve_style(base, resolving));
        }
    }
    merge_object(merged, value);
    return map_value(std::move(merged));
}

bool DescriptionBuilder::style_current(ResolvedStyle& style) const {
    if (!same_contextual_host_roots(style.contextual_host_roots))
        return false;
    const std::uint64_t invalidation_count = application_.host().invalidation_count();
    if (style.host_invalidation_count == invalidation_count)
        return true;
    for (const runtime::ExpressionHostDependency& dependency : style.host_values) {
        if (!host_read_current(runtime::canonical_host_dependency_path(dependency.path),
                               dependency))
            return false;
    }
    style.host_invalidation_count = invalidation_count;
    return true;
}

runtime::Value
DescriptionBuilder::resolve_named_style(const std::string_view name,
                                        std::set<std::string, std::less<>>& resolving) {
    // Whoever uses a style reads what the style read.
    const auto replay = [this](const ResolvedStyle& style) {
        if (runtime::ExpressionDependencyObserver* observer = expressions_->dependency_observer()) {
            for (const runtime::ExpressionHostDependency& dependency : style.host_values)
                observer->host(dependency);
        }
        return style.value;
    };
    if (const auto cached = styles_.find(name); cached != styles_.end()) {
        if (style_current(cached->second))
            return replay(cached->second);
        styles_.erase(cached);
    }
    const runtime::ProgramStyle* declaration = program().style(name);
    if (declaration == nullptr)
        return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{});
    const auto [resolving_entry, inserted] = resolving.emplace(name);
    if (!inserted)
        throw std::logic_error("validated portable IR contains a cyclic style inheritance chain");
    StyleDependencies dependencies;
    std::map<std::string, runtime::Value, std::less<>> merged;
    {
        ExpressionDependencyObserverRestore observe(*expressions_, &dependencies);
        for (const std::string& base : declaration->bases)
            merge_object(merged, resolve_named_style(base, resolving));
        // A style is a top-level declaration: it sees no locals, only the surface's host roots.
        runtime::ExpressionScope scope;
        scope.set_contextual_host_roots(contextual_host_roots_);
        for (const auto& [property, expression] : declaration->properties) {
            merged.insert_or_assign(property,
                                    require_value(evaluate(expression, scope),
                                                  program().expression(expression).source));
        }
    }
    resolving.erase(resolving_entry);
    const auto stored =
        styles_.insert_or_assign(std::string(name), ResolvedStyle{
                                                        map_value(std::move(merged)),
                                                        std::move(dependencies.host_values),
                                                        application_.host().invalidation_count(),
                                                        contextual_host_roots_,
                                                    });
    return replay(stored.first->second);
}

void DescriptionBuilder::normalize_layout(DescriptionNode::Properties& properties) {
    std::map<std::string, runtime::Value, std::less<>> merged;
    bool present = false;
    std::set<std::string, std::less<>> resolving;
    if (const auto style = properties.find("style");
        style != properties.end() && style->second.value() != nullptr) {
        merge_object(merged, resolve_style(*style->second.value(), resolving));
        present = true;
    }
    if (const auto layout = properties.find("layout");
        layout != properties.end() && layout->second.value() != nullptr) {
        merge_object(merged, *layout->second.value());
        present = true;
    }
    if (present)
        properties.insert_or_assign("$layout", ExpressionValue(map_value(std::move(merged))));
}

} // namespace strata::ui
