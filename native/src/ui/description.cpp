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

#include "ui/widget/description.hpp"

namespace strata::ui {
namespace {

using runtime::ExpressionValue;
using runtime::Symbol;

constexpr std::size_t maximum_component_cache_entries = 512U;

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

[[nodiscard]] std::string component_instance_path(const std::string_view parent,
                                                  const std::string_view type,
                                                  const std::optional<std::string>& key,
                                                  const std::string_view source_path) {
    std::string parent_path(parent);
    if (key.has_value()) {
        const std::size_t separator = parent_path.rfind('/');
        if (separator != std::string::npos &&
            parent_path.substr(separator + 1U).starts_with("for:")) {
            parent_path.resize(separator);
        }
    }
    const std::string identity =
        key.has_value() ? "key:" + *key : "call:" + std::string(source_path);
    return parent_path + "/component:" + std::string(type) + "/" + identity;
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

/** A loop's item (and index) over the scope it repeats in. */
[[nodiscard]] runtime::ScopeFrame item_frame(const Symbol item, const runtime::Value& value,
                                             const std::optional<Symbol> index,
                                             const std::size_t position) {
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

[[nodiscard]] bool same_inputs(const std::vector<ExpressionValue>& left,
                               const std::vector<ExpressionValue>& right) {
    return std::ranges::equal(left, right, runtime::same_expression_value);
}

class RepeaterIndexableSequence final : public runtime::IndexableSequence {
  public:
    using KeyFactory = std::function<std::string(const runtime::Value&, std::size_t)>;

    RepeaterIndexableSequence(const std::uint64_t generation, runtime::Value source,
                              std::optional<std::vector<std::size_t>> selection,
                              KeyFactory key_factory)
        : generation_(generation), source_(std::move(source)), selection_(std::move(selection)),
          key_factory_(std::move(key_factory)) {
        if (source_.list() == nullptr)
            throw std::invalid_argument("repeater sequence source must be a list");
        if (!key_factory_)
            throw std::invalid_argument("repeater sequence requires a key evaluator");
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
        return key_factory_(item_at(index), source_index_at(index));
    }

    [[nodiscard]] std::optional<std::size_t>
    index_of_key(const std::string_view key) const override {
        for (std::size_t index = 0U; index < count(); ++index) {
            if (key_at(index) == key)
                return index;
        }
        return std::nullopt;
    }

  private:
    std::uint64_t generation_;
    runtime::Value source_;
    std::optional<std::vector<std::size_t>> selection_;
    KeyFactory key_factory_;
};

struct RepeaterExpressionDependencies final : runtime::ExpressionDependencyObserver {
    void exclude(const Symbol name) {
        excluded.push_back(name);
    }

    void lexical(const Symbol name, const ExpressionValue& value) override {
        if (std::ranges::find(excluded, name) != excluded.end())
            return;
        lexical_values.insert_or_assign(name, runtime::capture_expression_dependency(value));
    }

    void host(const runtime::ExpressionHostDependency& dependency) override {
        host_values.insert_or_assign(runtime::canonical_host_dependency_path(dependency.path),
                                     dependency);
    }

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

struct ComponentExpressionDependencies final : runtime::ExpressionDependencyObserver {
    std::function<void(const runtime::ExpressionHostDependency&)> observe_host;
    runtime::ExpressionDependencyObserver* upstream = nullptr;

    void lexical(const Symbol name, const ExpressionValue& value) override {
        if (upstream != nullptr)
            upstream->lexical(name, value);
    }

    void host(const runtime::ExpressionHostDependency& dependency) override {
        observe_host(dependency);
        if (upstream != nullptr)
            upstream->host(dependency);
    }
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

void collect_slot_names(const std::shared_ptr<const DescriptionNode>& node,
                        std::set<std::string, std::less<>>& names) {
    if (node->type == "Slot") {
        const auto property = node->properties.find("name");
        const runtime::Value* value =
            property != node->properties.end() ? property->second.value() : nullptr;
        if (value != nullptr && value->string() != nullptr && !value->string()->empty()) {
            names.insert(*value->string());
        }
    }
    for (std::size_t index = 0U; index < node->children->size(); ++index) {
        collect_slot_names(node->children->at(index), names);
    }
}

[[nodiscard]] std::shared_ptr<const DescriptionNode>
project_slots(const std::shared_ptr<const DescriptionNode>& node,
              const std::map<std::string, std::vector<std::shared_ptr<const DescriptionNode>>,
                             std::less<>>& projected) {
    if (node->type == "Slot") {
        const auto property = node->properties.find("name");
        const runtime::Value* value =
            property != node->properties.end() ? property->second.value() : nullptr;
        if (value != nullptr && value->string() != nullptr) {
            const auto replacement = projected.find(*value->string());
            if (replacement != projected.end()) {
                auto resolved = std::make_shared<DescriptionNode>(*node);
                resolved->children =
                    std::make_shared<const EagerDescriptionChildren>(replacement->second);
                return resolved;
            }
        }
    }

    bool changed = false;
    std::vector<std::shared_ptr<const DescriptionNode>> children;
    children.reserve(node->children->size());
    for (std::size_t index = 0U; index < node->children->size(); ++index) {
        const std::shared_ptr<const DescriptionNode> source = node->children->at(index);
        std::shared_ptr<const DescriptionNode> resolved = project_slots(source, projected);
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

const std::string& DescriptionBuilder::Scope::instance_path() const noexcept {
    static const std::string none;
    return instance != nullptr ? *instance : none;
}

void DescriptionBuilder::Scope::set_instance(std::string path) {
    instance = std::make_shared<const std::string>(std::move(path));
    expressions.set_component_path(instance);
}

struct DescriptionBuilder::RepeaterIdentityEvaluationState final {
    RepeaterIdentityEvaluationState(runtime::ApplicationContext& application,
                                    const WidgetRegistry& source_widgets,
                                    std::shared_ptr<const runtime::RuntimeUnit> unit,
                                    Scope source_scope, const Symbol source_item,
                                    const std::optional<Symbol> source_index,
                                    const runtime::IdentityId source_identity)
        : widgets(source_widgets),
          evaluator(std::make_unique<DescriptionBuilder>(application, widgets)),
          scope(std::move(source_scope)), item(source_item), index(source_index),
          identity(source_identity) {
        evaluator->use_unit(unit);
        evaluator->contextual_host_roots_ = scope.expressions.shared_contextual_host_roots();
    }

    [[nodiscard]] std::string key(const runtime::Value& value, const std::size_t source_index) {
        std::scoped_lock lock(mutex);
        Scope item_scope = scope;
        item_scope.expressions.push(item_frame(item, value, index, source_index));
        // The complete domain was validated while the sequence generation was constructed.
        // Later observed-key queries are deterministic reads and do not leak counters or
        // diagnostics into an unrelated row materialization transaction.
        evaluator->diagnostics_.clear();
        evaluator->evaluated_expressions_ = 0U;
        return evaluator->evaluate_repeater_identity(identity, item_scope);
    }

    WidgetRegistry widgets;
    std::unique_ptr<DescriptionBuilder> evaluator;
    Scope scope;
    Symbol item;
    std::optional<Symbol> index;
    runtime::IdentityId identity;
    std::mutex mutex;
};

struct DescriptionBuilder::GeneratedRowEvaluationContext final {
    GeneratedRowEvaluationContext(runtime::ApplicationContext& application,
                                  const WidgetRegistry& source_widgets,
                                  const std::shared_ptr<const runtime::RuntimeUnit>& unit,
                                  std::shared_ptr<const runtime::HostRoots> contextual_host_roots,
                                  std::shared_ptr<const RetainedDescriptionSnapshot> retained)
        : widgets(source_widgets),
          evaluator(std::make_unique<DescriptionBuilder>(application, widgets)) {
        evaluator->use_unit(unit);
        evaluator->contextual_host_roots_ = std::move(contextual_host_roots);
        evaluator->retained_snapshot_ = std::move(retained);
    }

    void begin() {
        evaluator->diagnostics_.clear();
        evaluator->evaluated_expressions_ = 0U;
        evaluator->described_nodes_ = 0U;
        evaluator->current_layer_state_scopes_.clear();
    }

    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    finish(std::shared_ptr<const DescriptionNode> node, const std::size_t synthesized_nodes,
           std::string materialization_key) {
        if (node == nullptr)
            throw std::logic_error("generated row evaluator returned a null description");
        auto anchored = std::make_shared<DescriptionNode>(*node);
        if (!anchored->materialization_key.has_value())
            anchored->materialization_key = std::move(materialization_key);
        DescriptionMaterialization result{
            std::move(evaluator->current_layer_state_scopes_),
            std::move(evaluator->diagnostics_),
            evaluator->evaluated_expressions_,
            evaluator->described_nodes_,
        };
        if (synthesized_nodes > std::numeric_limits<std::size_t>::max() - result.described_nodes) {
            throw std::overflow_error("generated row described-node count exhausted");
        }
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
        return anchored;
    }

    WidgetRegistry widgets;
    std::unique_ptr<DescriptionBuilder> evaluator;
};

struct DescriptionBuilder::LazyRowEvaluationState final {
    LazyRowEvaluationState(runtime::ApplicationContext& application,
                           const WidgetRegistry& source_widgets,
                           const std::shared_ptr<const runtime::RuntimeUnit>& unit,
                           Scope source_scope, const Symbol source_item,
                           const std::optional<Symbol> source_index,
                           const runtime::BlockId source_block,
                           std::shared_ptr<const RetainedDescriptionSnapshot> retained)
        : evaluation(application, source_widgets, unit,
                     source_scope.expressions.shared_contextual_host_roots(), std::move(retained)),
          scope(std::move(source_scope)), item(source_item), index(source_index),
          block(source_block) {}

    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    materialize(const runtime::IndexableSequence& sequence, const std::size_t lazy_index) {
        evaluation.begin();
        const runtime::Value& value = sequence.item_at(lazy_index);
        const std::size_t source_index = sequence.source_index_at(lazy_index);
        const std::string canonical_key = sequence.key_at(lazy_index);
        Scope item_scope = scope;
        item_scope.expressions.push(item_frame(item, value, index, source_index));
        item_scope.set_instance(item_scope.instance_path() + "/" + std::string(item.name()) + ":" +
                                canonical_key);
        std::vector<std::shared_ptr<const DescriptionNode>> nodes =
            evaluation.evaluator->build_block(block, item_scope);
        if (nodes.size() != 1U) {
            throw std::logic_error(
                "validated Repeater identity selected a row body that did not produce one root");
        }
        auto anchored = std::make_shared<DescriptionNode>(*nodes.front());
        anchored->key = canonical_key;
        return evaluation.finish(std::move(anchored), 0U, canonical_key);
    }

    GeneratedRowEvaluationContext evaluation;
    Scope scope;
    Symbol item;
    std::optional<Symbol> index;
    runtime::BlockId block;
};

struct DescriptionBuilder::WidgetRowEvaluationState final {
    WidgetRowEvaluationState(runtime::ApplicationContext& application,
                             const WidgetRegistry& source_widgets,
                             const std::shared_ptr<const runtime::RuntimeUnit>& unit,
                             Scope source_scope, WidgetGeneratedChildHook source_factory,
                             std::shared_ptr<const RetainedDescriptionSnapshot> retained)
        : evaluation(application, source_widgets, unit,
                     source_scope.expressions.shared_contextual_host_roots(), std::move(retained)),
          caller(std::move(source_scope)), factory(std::move(source_factory)),
          actions(&application.bundle()->action_registry()) {}

    [[nodiscard]] std::shared_ptr<const DescriptionNode> materialize(const std::size_t index) {
        evaluation.begin();
        WidgetDescriptionExpansion item;
        WidgetDescriptionScope item_scope(
            item, caller.instance_path(), *actions, evaluation.widgets, nullptr,
            [this](const std::string_view component, std::string key,
                   WidgetTemplateArguments arguments) {
                return evaluation.evaluator->build_component_template(component, std::move(key),
                                                                      std::move(arguments), caller);
            },
            {});
        std::shared_ptr<const DescriptionNode> row = factory(item_scope, index);
        const std::string materialization_key = row != nullptr && row->key.has_value()
                                                    ? *row->key
                                                    : "widget-generated-" + std::to_string(index);
        return evaluation.finish(std::move(row), item.synthesized_nodes, materialization_key);
    }

    GeneratedRowEvaluationContext evaluation;
    Scope caller;
    WidgetGeneratedChildHook factory;
    const runtime::RuntimeActionRegistry* actions;
};

DescriptionBuilder::DescriptionBuilder(runtime::ApplicationContext& application)
    : application_(application), owned_widgets_(std::make_unique<WidgetRegistry>()),
      widgets_(*owned_widgets_), expressions_(std::make_unique<runtime::ExpressionRuntime>(
                                     application.host(), application.bundle()->action_registry())) {
}

DescriptionBuilder::DescriptionBuilder(runtime::ApplicationContext& application,
                                       const WidgetRegistry& widgets)
    : application_(application), owned_widgets_(), widgets_(widgets),
      expressions_(std::make_unique<runtime::ExpressionRuntime>(
          application.host(), application.bundle()->action_registry())) {}

DescriptionBuildResult DescriptionBuilder::build(const runtime::LayerRole role,
                                                 const std::string_view name) {
    const LayerDescriptionRequest request{role, std::string(name)};
    DescriptionLayersBuildResult result = build_layers(std::span(&request, 1U));
    return DescriptionBuildResult{
        std::move(result.roots.front()),
        std::move(result.diagnostics),
        result.evaluated_expressions,
        result.described_nodes,
    };
}

void DescriptionBuilder::set_retained_tree(const RetainedTree* const tree) {
    retained_snapshot_ = tree != nullptr ? tree->description_snapshot() : nullptr;
}

bool DescriptionBuilder::observes_retained_value(const RetainedNode& node,
                                                 const std::string_view name,
                                                 const bool include_lazy_snapshots) const {
    if (retained_snapshot_ == nullptr || layer_cache_.empty())
        return true;
    const auto effects_observe = [this, &node, name,
                                  include_lazy_snapshots](const ComponentEffects& effects) {
        if (include_lazy_snapshots && effects.captures_retained_snapshot)
            return true;
        return std::ranges::any_of(
            effects.retained_values, [this, &node, name](const RetainedValueEffects& effect) {
                if (!effect.values.contains(name))
                    return false;
                const RetainedDescriptionSnapshot::Node* const retained =
                    retained_widget(effect.query);
                return retained != nullptr && retained->identity == node.identity();
            });
    };
    if (std::ranges::any_of(layer_cache_, [&effects_observe](const auto& entry) {
            return effects_observe(entry.second.effects);
        })) {
        return true;
    }
    return std::ranges::any_of(component_cache_, [&effects_observe](const auto& entry) {
        return effects_observe(entry.second.effects);
    });
}

void DescriptionBuilder::set_contextual_host_roots(
    std::map<std::string, runtime::Value, std::less<>> roots) {
    // Unchanged roots keep their identity, which is what cache entries compare first.
    if (contextual_host_roots_ != nullptr && *contextual_host_roots_ == roots)
        return;
    if (contextual_host_roots_ == nullptr && roots.empty())
        return;
    contextual_host_roots_ = std::make_shared<const runtime::HostRoots>(std::move(roots));
}

void DescriptionBuilder::use_unit(const std::shared_ptr<const runtime::RuntimeUnit>& unit) {
    if (unit_ == unit)
        return;
    component_cache_.clear();
    component_ids_.clear();
    layer_cache_.clear();
    styles_.clear();
    component_cache_epoch_ = 0U;
    expressions_->clear_caches();
    unit_ = unit;
    call_widgets_.assign(unit_ != nullptr ? program().call_count() : 0U, std::nullopt);
    call_widgets_revision_ = widgets_.revision();
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

DescriptionLayersBuildResult
DescriptionBuilder::build_layers(const std::span<const LayerDescriptionRequest> layers) {
    if (layers.empty())
        throw std::invalid_argument("description layer list must not be empty");
    const std::shared_ptr<const runtime::RuntimeUnit>& active = application_.active_unit();
    if (active == nullptr)
        throw std::logic_error("description build requires an active runtime unit");
    if (component_cache_epoch_ == std::numeric_limits<std::uint64_t>::max())
        use_unit(nullptr);
    use_unit(active);
    ++component_cache_epoch_;
    diagnostics_.clear();
    evaluated_expressions_ = 0U;
    described_nodes_ = 0U;
    application_.clear_state_scope_bindings();
    std::vector<std::shared_ptr<const DescriptionNode>> roots;
    std::vector<runtime::StateScopeSet> layer_state_scopes;
    roots.reserve(layers.size());
    layer_state_scopes.reserve(layers.size());
    for (const LayerDescriptionRequest& layer : layers) {
        current_layer_state_scopes_.clear();
        roots.push_back(build_layer(layer.role, layer.name));
        layer_state_scopes.push_back(current_layer_state_scopes_);
    }
    if (component_cache_.size() > maximum_component_cache_entries) {
        std::vector<std::pair<std::uint64_t, ComponentId>> eviction_candidates;
        eviction_candidates.reserve(component_cache_.size());
        for (const auto& [id, entry] : component_cache_) {
            if (entry.visited_epoch != component_cache_epoch_)
                eviction_candidates.emplace_back(entry.last_used_epoch, id);
        }
        std::ranges::sort(eviction_candidates);
        const std::size_t removal_count = std::min(
            component_cache_.size() - maximum_component_cache_entries, eviction_candidates.size());
        for (std::size_t index = 0U; index < removal_count; ++index) {
            forget_component(eviction_candidates[index].second);
        }
    }
    return DescriptionLayersBuildResult{
        std::move(roots),        std::move(layer_state_scopes),
        std::move(diagnostics_), evaluated_expressions_,
        described_nodes_,
    };
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::build_layer(const runtime::LayerRole role, const std::string_view name) {
    const runtime::ProgramLayer* declaration =
        role == runtime::LayerRole::screen ? program().screen(name) : program().overlay(name);
    if (declaration == nullptr)
        throw std::invalid_argument("requested layer declaration is not active");
    const std::string& source_path = declaration->path;
    const std::string cache_key =
        std::string(role == runtime::LayerRole::screen ? "screen\n" : "overlay\n") +
        std::string(name);
    if (auto cached = layer_cache_.find(cache_key);
        cached != layer_cache_.end() && cached->second.role == role &&
        cached->second.name == name && cached->second.source_path == source_path &&
        same_contextual_host_roots(cached->second.contextual_host_roots)) {
        std::map<const DescriptionNode*, std::shared_ptr<const DescriptionNode>> replacements;
        bool valid = true;
        for (const ComponentId child : cached->second.effects.direct_descendants) {
            const auto child_before = component_cache_.find(child);
            if (child_before == component_cache_.end()) {
                valid = false;
                break;
            }
            const std::shared_ptr<const DescriptionNode> previous = child_before->second.root;
            if (refresh_component_cache_entry(child) == ComponentRefreshResult::invalid) {
                valid = false;
                break;
            }
            const auto child_after = component_cache_.find(child);
            if (child_after == component_cache_.end()) {
                valid = false;
                break;
            }
            if (child_after->second.root != previous)
                replacements.insert_or_assign(previous.get(), child_after->second.root);
        }
        if (valid) {
            const bool direct_current = component_effects_current(
                cached->second.effects, cached->second.host_invalidation_count,
                cached->second.contextual_host_roots);
            if (replacements.empty() && direct_current) {
                cached->second.host_invalidation_count = application_.host().invalidation_count();
                replay_component_effects(cached->second.effects);
                return cached->second.root;
            }
            if (direct_current) {
                std::shared_ptr<const DescriptionNode> patched =
                    replace_component_subtrees(cached->second.root, replacements);
                if (patched != nullptr && aggregate_component_effects(cached->second.effects)) {
                    cached->second.host_invalidation_count =
                        application_.host().invalidation_count();
                    cached->second.root = patched;
                    replay_component_effects(cached->second.effects);
                    return patched;
                }
            }
        }
    }

    Scope scope;
    scope.expressions.set_contextual_host_roots(contextual_host_roots_);
    scope.set_instance(declaration->declaration_scope);
    scope.declaration_scope = declaration->declaration_scope;
    ComponentEffects effects;
    component_effect_stack_.push_back(&effects);
    ComponentExpressionDependencies dependencies;
    dependencies.observe_host = [this](const runtime::ExpressionHostDependency& dependency) {
        observe_host_dependency(dependency);
    };
    ExpressionDependencyObserverRestore dependency_observer(*expressions_, &dependencies);
    dependencies.upstream = dependency_observer.previous();
    std::vector<std::shared_ptr<const DescriptionNode>> roots;
    try {
        roots = build_block(declaration->body, scope);
    } catch (...) {
        component_effect_stack_.pop_back();
        throw;
    }
    component_effect_stack_.pop_back();
    auto declaration_children = std::make_shared<const EagerDescriptionChildren>(std::move(roots));
    auto root = DescriptionNode::create(role == runtime::LayerRole::screen ? "$screen" : "$overlay",
                                        std::nullopt, source_path, declaration->declaration_scope,
                                        {}, std::move(declaration_children));
    ++described_nodes_;
    if (const auto previous = layer_cache_.find(cache_key); previous != layer_cache_.end())
        root = share_unchanged_description(previous->second.root, root);
    layer_cache_.insert_or_assign(cache_key, LayerCacheEntry{
                                                 role,
                                                 std::string(name),
                                                 source_path,
                                                 application_.host().invalidation_count(),
                                                 contextual_host_roots_,
                                                 std::move(effects),
                                                 root,
                                             });
    return root;
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
            const std::string slot_type = declaration.type_id == "dsl.unknown"
                                              ? std::string(initial.state_type_id())
                                              : declaration.type_id;
            const runtime::StateSlot slot{
                name,
                slot_type,
                initial,
                std::string(declaring.declaration_scope),
            };
            const runtime::StateAddress address{address_scope, name};
            if (declaration.persistence_key.has_value() &&
                application_.state().find(address) == nullptr) {
                if (const runtime::Value* persisted =
                        application_.durability().application_value(*declaration.persistence_key);
                    persisted != nullptr) {
                    if (declaration.schema != nullptr && declaration.schema->accepts(*persisted)) {
                        static_cast<void>(application_.state().write(address, slot, *persisted));
                    } else {
                        application_.services().report(runtime::RuntimeDiagnostic{
                            "STRATA.DURABILITY.TYPE_MISMATCH",
                            "Persisted value '" + *declaration.persistence_key +
                                "' no longer matches state '" + name + "' and was discarded.",
                            declaring.instance_path(),
                            slot.type_id,
                            runtime::DiagnosticSeverity::warning,
                            std::nullopt,
                        });
                        static_cast<void>(application_.durability().erase_application_value(
                            *declaration.persistence_key));
                    }
                }
            }
            const runtime::Value& state = application_.state().read(address, slot);
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
                std::string segment = repeated_item_segment(values->values[index], index);
                const std::size_t occurrence = repeated_segments[segment]++;
                if (occurrence != 0U)
                    segment += ":" + std::to_string(occurrence);
                item_scope.set_instance(item_scope.instance_path() + "/" + std::string(item_name) +
                                        ":" + segment);
                auto iteration = build_block(statement.block, item_scope);
                for (std::size_t item = 0U; item < iteration.size(); ++item) {
                    const bool anchor = item == 0U;
                    const bool staggered = iteration[item]->properties.contains("stagger");
                    if (!anchor && !staggered)
                        continue;
                    auto annotated = std::make_shared<DescriptionNode>(*iteration[item]);
                    if (anchor) {
                        annotated->materialization_key =
                            lazy_item_key(values->values[index], index);
                    }
                    if (staggered) {
                        annotated->properties.insert_or_assign(
                            "$loopPosition",
                            ExpressionValue(runtime::Value(static_cast<double>(position))));
                    }
                    iteration[item] = std::move(annotated);
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
    const runtime::BlockId block, const Scope& scope,
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
    const std::shared_ptr<const runtime::IndexableSequence> previous_sequence =
        retained_widget != nullptr ? retained_widget->virtual_sequence.lock() : nullptr;
    const DescriptionSequenceGeneration* const previous_stamp =
        retained_widget != nullptr && retained_widget->virtual_sequence_generation.has_value()
            ? &*retained_widget->virtual_sequence_generation
            : nullptr;
    {
        // Source reads belong to the retained generation just as filter/identity reads do. The
        // loop domain is excluded only after source evaluation so an outer binding with the same
        // name is still traced when it participates in the source expression.
        RepeaterExpressionDependencies dependencies;
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
        } else {
            dependencies.exclude(item_name);
            if (index_name.has_value())
                dependencies.exclude(*index_name);
            std::optional<std::vector<std::size_t>> selection;
            if (statement.filter != runtime::no_program_id) {
                selection.emplace();
                selection->reserve(values->values.size());
            }
            std::map<std::string, std::size_t, std::less<>> first_source_index_by_key;
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
                const std::string canonical_key =
                    evaluate_repeater_identity(statement.identity, item_scope);
                const auto [duplicate, inserted] =
                    first_source_index_by_key.emplace(canonical_key, index);
                if (!inserted) {
                    throw std::invalid_argument("Repeater root key '" + canonical_key +
                                                "' is duplicated at source indexes " +
                                                std::to_string(duplicate->second) + " and " +
                                                std::to_string(index));
                }
                if (selection.has_value())
                    selection->push_back(index);
            }
            stamp.lexical_dependencies = std::move(dependencies.lexical_values);
            stamp.host_dependencies = std::move(dependencies.host_values);
            const std::uint64_t previous_generation =
                previous_sequence != nullptr ? previous_sequence->generation() : 0U;
            if (previous_generation == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("repeater sequence generation exhausted");
            // Later key queries read exactly what this generation read.
            Scope identity_scope = scope;
            identity_scope.expressions.set_host_dependency_overrides(
                std::make_shared<const runtime::FrozenHostReads>(stamp.host_dependencies));
            runtime::ScopeFrame frozen;
            for (const auto& [name, value] : stamp.lexical_dependencies) {
                // A source may read an outer binding the loop's own names shadow: the per-item
                // domain is never frozen.
                if (name == item_name || (index_name.has_value() && name == *index_name))
                    continue;
                frozen.bindings.push_back(
                    runtime::ScopeBinding{name, true, runtime::ScopeStateBinding::inherit,
                                          runtime::restore_expression_dependency(value), nullptr});
            }
            if (!frozen.bindings.empty())
                identity_scope.expressions.push(std::move(frozen));
            auto identity_evaluation = std::make_shared<RepeaterIdentityEvaluationState>(
                application_, widgets_, unit_, std::move(identity_scope), item_name, index_name,
                statement.identity);
            sequence = std::make_shared<const RepeaterIndexableSequence>(
                previous_generation + 1U, source_items, std::move(selection),
                [identity_evaluation = std::move(identity_evaluation)](
                    const runtime::Value& item, const std::size_t source_index) {
                    return identity_evaluation->key(item, source_index);
                });
        }
    }

    capture_retained_snapshot();
    auto evaluation =
        std::make_shared<LazyRowEvaluationState>(application_, widgets_, unit_, scope, item_name,
                                                 index_name, statement.block, retained_snapshot_);
    auto source = std::make_shared<const GeneratedDescriptionChildren>(
        sequence->count(),
        [evaluation = std::move(evaluation), sequence](const std::size_t lazy_index) {
            return evaluation->materialize(*sequence, lazy_index);
        });
    return RepeaterChildren{std::move(source), std::move(sequence), std::move(stamp)};
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
    RepeaterChildren repeater_children;
    if (type == "Repeater" && call.children != runtime::no_program_id) {
        observe_retained_sequence(retained_query, retained);
        repeater_children = build_repeater_children(call.children, scope, retained);
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
        component_effect_stack_.empty()
            ? WidgetRetainedDependencyObserver{}
            : WidgetRetainedDependencyObserver{
                  [this, retained_query](const std::string_view name,
                                         const runtime::Value* const value) {
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
        capture_retained_snapshot();
        auto evaluation = std::make_shared<WidgetRowEvaluationState>(
            application_, widgets_, unit_, scope, generated->factory, retained_snapshot_);
        generated_children = std::make_shared<const GeneratedDescriptionChildren>(
            generated->count, [evaluation = std::move(evaluation)](const std::size_t index) {
                return evaluation->materialize(index);
            });
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
    return result;
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
        component, component_instance_path(scope.instance_path(), type, key, source_path), scope,
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

    const std::string cache_key = type + "\n" + instance.instance_path();
    const ComponentId id = component_id(cache_key);
    for (ComponentEffects* const component_effect : component_effect_stack_)
        add_descendant(component_effect->descendants, id);
    if (!component_effect_stack_.empty()) {
        std::vector<ComponentId>& direct = component_effect_stack_.back()->direct_descendants;
        if (std::ranges::find(direct, id) == direct.end())
            direct.push_back(id);
    }
    std::shared_ptr<const DescriptionNode> component_root;
    std::shared_ptr<const DescriptionNode> previous_root;
    auto cached = component_cache_.find(id);
    bool current = false;
    if (cached != component_cache_.end() &&
        cached->second.component_index == call.component_index &&
        cached->second.source_path == source_path && same_inputs(cached->second.inputs, inputs) &&
        same_contextual_host_roots(cached->second.contextual_host_roots)) {
        const bool refreshing = cached->second.refreshing;
        const ComponentRefreshResult refreshed = refresh_component_cache_entry(id);
        cached = component_cache_.find(id);
        // A refresh leaves a surviving entry current for the inputs it was matched on,
        // unless it was already refreshing and so returned without looking.
        current =
            cached != component_cache_.end() && refreshed != ComponentRefreshResult::invalid &&
            (!refreshing || component_cache_entry_current(cached->second, source_path, inputs));
    }
    if (current) {
        cached->second.host_invalidation_count = application_.host().invalidation_count();
        cached->second.last_used_epoch = component_cache_epoch_;
        cached->second.visited_epoch = component_cache_epoch_;
        replay_component_effects(cached->second.effects);
        component_root = cached->second.root;
    } else if (cached != component_cache_.end()) {
        previous_root = cached->second.root;
    }
    if (component_root == nullptr) {
        ComponentEffects effects;
        const std::size_t diagnostics_before = diagnostics_.size();
        component_root = share_unchanged_description(
            previous_root, build_component_body(call.component_index, instance, effects));
        if (diagnostics_.size() == diagnostics_before) {
            component_cache_.insert_or_assign(id, ComponentCacheEntry{
                                                      type,
                                                      call.component_index,
                                                      source_path,
                                                      std::move(inputs),
                                                      application_.host().invalidation_count(),
                                                      component_cache_epoch_,
                                                      contextual_host_roots_,
                                                      std::move(effects),
                                                      component_root,
                                                      std::move(instance),
                                                      cache_key,
                                                      component_cache_epoch_,
                                                      false,
                                                  });
        } else {
            for (ComponentEffects* const component_effect : component_effect_stack_) {
                std::erase(component_effect->direct_descendants, id);
                std::erase(component_effect->descendants, id);
            }
            forget_component(id);
            absorb_uncached_component_effects(effects);
        }
    }
    if (call.children != runtime::no_program_id) {
        std::set<std::string, std::less<>> declared_slots;
        collect_slot_names(component_root, declared_slots);
        if (!raw_content.empty()) {
            const std::string shorthand = declared_slots.size() == 1U ? *declared_slots.begin()
                                          : declared_slots.contains("content")
                                              ? std::string("content")
                                              : std::string{};
            if (!shorthand.empty())
                projected_content.insert_or_assign(shorthand, std::move(raw_content));
        }
        for (auto content = projected_content.begin(); content != projected_content.end();) {
            if (!declared_slots.contains(content->first))
                content = projected_content.erase(content);
            else
                ++content;
        }
        if (!projected_content.empty())
            component_root = project_slots(component_root, projected_content);
    }
    if (behaviors.empty())
        return component_root;

    auto expanded = std::make_shared<DescriptionNode>(*component_root);
    for (DescriptionBehavior& behavior : behaviors) {
        const auto duplicate =
            std::ranges::find(expanded->behaviors, behavior.id, &DescriptionBehavior::id);
        if (duplicate != expanded->behaviors.end())
            throw std::logic_error("component call attaches a duplicate root behavior");
        expanded->behaviors.push_back(std::move(behavior));
    }
    return expanded;
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
                                          const std::string_view address_scope,
                                          const bool replayed) {
    const runtime::StateAddressView address{runtime_scope, state_name};
    const auto record =
        [&](std::map<runtime::StateAddress, StateBindingEffect, std::less<>>& bindings) {
            const auto found = bindings.lower_bound(address);
            if (found != bindings.end() && found->first == address) {
                if (found->second.declaration_scope != declaration_scope)
                    found->second.declaration_scope = declaration_scope;
                if (found->second.address_scope != address_scope)
                    found->second.address_scope = address_scope;
                return;
            }
            bindings.emplace_hint(
                found, runtime::StateAddress{std::string(runtime_scope), std::string(state_name)},
                StateBindingEffect{std::string(declaration_scope), std::string(address_scope)});
        };
    for (ComponentEffects* const component : component_effect_stack_) {
        record(component->state_bindings);
    }
    if (!replayed && !component_effect_stack_.empty()) {
        record(component_effect_stack_.back()->local_state_bindings);
    }
    application_.bind_state_scope(runtime_scope, state_name, declaration_scope, address_scope);
}

void DescriptionBuilder::own_state_scope(const std::string_view scope, const bool replayed) {
    current_layer_state_scopes_.insert(std::string(scope));
    application_.state().mark_owned_scope(std::string(scope));
    for (ComponentEffects* const component : component_effect_stack_) {
        component->owned_state_scopes.insert(std::string(scope));
    }
    if (!replayed && !component_effect_stack_.empty()) {
        component_effect_stack_.back()->local_owned_state_scopes.insert(std::string(scope));
    }
}

void DescriptionBuilder::observe_state_value(const runtime::StateAddress& address,
                                             const runtime::Value& value) {
    if (!component_effect_stack_.empty())
        component_effect_stack_.back()->state_values.insert_or_assign(address, value);
}

void DescriptionBuilder::observe_host_dependency(
    const runtime::ExpressionHostDependency& dependency) {
    if (component_effect_stack_.empty())
        return;
    component_effect_stack_.back()->host_values.insert_or_assign(
        runtime::canonical_host_dependency_path(dependency.path), dependency);
}

void DescriptionBuilder::observe_retained_value(const RetainedQuery& query,
                                                const std::string_view name,
                                                const runtime::Value* const value) {
    const std::optional<runtime::Value> observed =
        value != nullptr ? std::optional(*value) : std::nullopt;
    if (component_effect_stack_.empty())
        return;
    ComponentEffects& component = *component_effect_stack_.back();
    auto effect = std::ranges::find(component.retained_values, query, &RetainedValueEffects::query);
    if (effect == component.retained_values.end()) {
        component.retained_values.push_back(RetainedValueEffects{query, {}});
        effect = std::prev(component.retained_values.end());
    }
    effect->values.insert_or_assign(std::string(name), observed);
}

void DescriptionBuilder::observe_retained_sequence(
    const RetainedQuery& query, const RetainedDescriptionSnapshot::Node* const retained) {
    if (component_effect_stack_.empty())
        return;
    const std::shared_ptr<const runtime::IndexableSequence> sequence =
        retained != nullptr ? retained->virtual_sequence.lock() : nullptr;
    const std::optional<DescriptionSequenceGeneration> generation =
        retained != nullptr ? retained->virtual_sequence_generation : std::nullopt;
    ComponentEffects& component = *component_effect_stack_.back();
    auto effect =
        std::ranges::find(component.retained_sequences, query, &RetainedSequenceEffect::query);
    if (effect == component.retained_sequences.end()) {
        component.retained_sequences.push_back(RetainedSequenceEffect{query, sequence, generation});
    } else {
        effect->sequence = sequence;
        effect->generation = generation;
    }
}

void DescriptionBuilder::capture_retained_snapshot() {
    for (ComponentEffects* const component : component_effect_stack_) {
        component->captures_retained_snapshot = true;
        component->retained_snapshot = retained_snapshot_;
    }
    if (!component_effect_stack_.empty())
        component_effect_stack_.back()->local_captures_retained_snapshot = true;
}

std::shared_ptr<const DescriptionNode>
DescriptionBuilder::build_component_body(const std::uint32_t component, const Scope& scope,
                                         ComponentEffects& effects) {
    component_effect_stack_.push_back(&effects);
    ComponentExpressionDependencies dependencies;
    dependencies.observe_host = [this](const runtime::ExpressionHostDependency& dependency) {
        observe_host_dependency(dependency);
    };
    ExpressionDependencyObserverRestore dependency_observer(*expressions_, &dependencies);
    dependencies.upstream = dependency_observer.previous();
    std::vector<std::shared_ptr<const DescriptionNode>> roots;
    try {
        roots = build_block(program().component(component).body, scope);
    } catch (...) {
        component_effect_stack_.pop_back();
        throw;
    }
    component_effect_stack_.pop_back();
    if (roots.size() != 1U)
        throw std::logic_error("validated component body must describe exactly one root node");
    return std::move(roots.front());
}

std::shared_ptr<const DescriptionNode> DescriptionBuilder::replace_component_subtrees(
    const std::shared_ptr<const DescriptionNode>& root,
    const std::map<const DescriptionNode*, std::shared_ptr<const DescriptionNode>>& replacements) {
    if (root == nullptr || replacements.empty())
        return root;
    std::set<const DescriptionNode*> applied;
    const auto replace = [&replacements,
                          &applied](const auto& self,
                                    const std::shared_ptr<const DescriptionNode>& current)
        -> std::shared_ptr<const DescriptionNode> {
        if (const auto replacement = replacements.find(current.get());
            replacement != replacements.end()) {
            applied.insert(replacement->first);
            return replacement->second;
        }
        if (current->materialization.has_value())
            return current;
        bool changed = false;
        std::vector<std::shared_ptr<const DescriptionNode>> children;
        children.reserve(current->children->size());
        for (std::size_t index = 0U; index < current->children->size(); ++index) {
            const std::shared_ptr<const DescriptionNode> child = current->children->at(index);
            std::shared_ptr<const DescriptionNode> next = self(self, child);
            changed = changed || next != child;
            children.push_back(std::move(next));
        }
        if (!changed)
            return current;
        auto result = std::make_shared<DescriptionNode>(*current);
        result->children = std::make_shared<const EagerDescriptionChildren>(std::move(children));
        result->children_replaced_from = current;
        return result;
    };
    std::shared_ptr<const DescriptionNode> result = replace(replace, root);
    // A loop or slot projection may have cloned a component root to annotate it. In that case a
    // partial pointer rewrite would leave stale descendants, so the caller must use its rebuild.
    if (applied.size() != replacements.size())
        return nullptr;
    return result;
}

DescriptionBuilder::ComponentRefreshResult
DescriptionBuilder::refresh_component_cache_entry(const ComponentId id) {
    auto found = component_cache_.find(id);
    if (found == component_cache_.end())
        return ComponentRefreshResult::invalid;
    if (found->second.refreshing)
        return ComponentRefreshResult::unchanged;
    found->second.refreshing = true;
    struct RefreshGuard final {
        std::unordered_map<ComponentId, ComponentCacheEntry>& cache;
        ComponentId id;
        ~RefreshGuard() {
            if (const auto entry = cache.find(id); entry != cache.end())
                entry->second.refreshing = false;
        }
    } guard{component_cache_, id};

    // Entries are nodes: refreshing others (which may add entries) leaves this reference valid.
    ComponentCacheEntry& entry = found->second;
    std::map<const DescriptionNode*, std::shared_ptr<const DescriptionNode>> replacements;
    for (const ComponentId child : entry.effects.direct_descendants) {
        const auto child_before = component_cache_.find(child);
        if (child_before == component_cache_.end())
            return ComponentRefreshResult::invalid;
        const std::shared_ptr<const DescriptionNode> previous = child_before->second.root;
        const ComponentRefreshResult refreshed = refresh_component_cache_entry(child);
        const auto child_after = component_cache_.find(child);
        if (refreshed == ComponentRefreshResult::invalid || child_after == component_cache_.end())
            return ComponentRefreshResult::invalid;
        if (child_after->second.root != previous)
            replacements.insert_or_assign(previous.get(), child_after->second.root);
    }

    // The inputs are the entry's own: only what its body read can have changed.
    const bool direct_current = component_effects_current(
        entry.effects, entry.host_invalidation_count, entry.contextual_host_roots);
    if (replacements.empty() && direct_current)
        return ComponentRefreshResult::unchanged;
    // Only descendants changed: the body would describe the same nodes around them, so their new
    // subtrees are patched in and the aggregate is recomputed from the descendants' entries.
    if (direct_current) {
        std::shared_ptr<const DescriptionNode> patched =
            replace_component_subtrees(entry.root, replacements);
        if (patched != nullptr && aggregate_component_effects(entry.effects)) {
            entry.host_invalidation_count = application_.host().invalidation_count();
            entry.last_used_epoch = component_cache_epoch_;
            entry.root = std::move(patched);
            return ComponentRefreshResult::changed;
        }
    }

    const std::size_t diagnostics_before = diagnostics_.size();
    ComponentEffects effects;
    std::shared_ptr<const DescriptionNode> rebuilt =
        build_component_body(entry.component_index, entry.rebuild_scope, effects);
    if (diagnostics_.size() != diagnostics_before) {
        forget_component(id);
        return ComponentRefreshResult::invalid;
    }
    entry.host_invalidation_count = application_.host().invalidation_count();
    entry.last_used_epoch = component_cache_epoch_;
    entry.effects = std::move(effects);
    entry.root = share_unchanged_description(entry.root, rebuilt);
    return ComponentRefreshResult::changed;
}

void DescriptionBuilder::replay_component_effects(const ComponentEffects& effects) {
    for (const ComponentId id : effects.descendants) {
        if (const auto entry = component_cache_.find(id); entry != component_cache_.end())
            entry->second.visited_epoch = component_cache_epoch_;
    }
    for (ComponentEffects* const component : component_effect_stack_) {
        add_descendants(component->descendants, effects.descendants);
    }
    for (const std::string& scope : effects.owned_state_scopes)
        own_state_scope(scope, true);
    for (const auto& [binding_address, binding] : effects.state_bindings) {
        bind_state_scope(binding_address.scope, binding_address.name, binding.declaration_scope,
                         binding.address_scope, true);
    }
}

bool DescriptionBuilder::aggregate_component_effects(ComponentEffects& effects) const {
    effects.state_bindings = effects.local_state_bindings;
    effects.owned_state_scopes = effects.local_owned_state_scopes;
    effects.captures_retained_snapshot = effects.local_captures_retained_snapshot;
    effects.descendants.clear();
    for (const ComponentId child_id : effects.direct_descendants) {
        const auto child = component_cache_.find(child_id);
        if (child == component_cache_.end())
            return false;
        const ComponentEffects& descendant = child->second.effects;
        effects.descendants.push_back(child_id);
        effects.descendants.insert(effects.descendants.end(), descendant.descendants.begin(),
                                   descendant.descendants.end());
        for (const auto& [address, binding] : descendant.state_bindings) {
            effects.state_bindings.insert_or_assign(address, binding);
        }
        effects.owned_state_scopes.insert(descendant.owned_state_scopes.begin(),
                                          descendant.owned_state_scopes.end());
        effects.captures_retained_snapshot =
            effects.captures_retained_snapshot || descendant.captures_retained_snapshot;
    }
    std::ranges::sort(effects.descendants);
    const auto duplicates = std::ranges::unique(effects.descendants);
    effects.descendants.erase(duplicates.begin(), duplicates.end());
    effects.retained_snapshot = effects.captures_retained_snapshot ? retained_snapshot_ : nullptr;
    return true;
}

DescriptionBuilder::ComponentId DescriptionBuilder::component_id(const std::string& cache_key) {
    const auto [found, inserted] = component_ids_.try_emplace(cache_key, next_component_id_);
    if (inserted)
        ++next_component_id_;
    return found->second;
}

void DescriptionBuilder::forget_component(const ComponentId id) {
    const auto found = component_cache_.find(id);
    if (found == component_cache_.end())
        return;
    component_ids_.erase(found->second.cache_key);
    component_cache_.erase(found);
}

void DescriptionBuilder::add_descendant(std::vector<ComponentId>& sorted, const ComponentId id) {
    const auto position = std::ranges::lower_bound(sorted, id);
    if (position == sorted.end() || *position != id)
        sorted.insert(position, id);
}

void DescriptionBuilder::add_descendants(std::vector<ComponentId>& sorted,
                                         const std::vector<ComponentId>& more) {
    if (more.empty())
        return;
    std::vector<ComponentId> merged;
    merged.reserve(sorted.size() + more.size());
    std::ranges::set_union(sorted, more, std::back_inserter(merged));
    sorted = std::move(merged);
}

void DescriptionBuilder::absorb_uncached_component_effects(const ComponentEffects& effects) {
    if (component_effect_stack_.empty())
        return;
    ComponentEffects& parent = *component_effect_stack_.back();
    parent.host_values.insert(effects.host_values.begin(), effects.host_values.end());
    parent.state_values.insert(effects.state_values.begin(), effects.state_values.end());
    parent.state_bindings.insert(effects.state_bindings.begin(), effects.state_bindings.end());
    parent.owned_state_scopes.insert(effects.owned_state_scopes.begin(),
                                     effects.owned_state_scopes.end());
    // An uncached component has no entry to aggregate from later, so what it contributed itself
    // becomes its parent's own contribution.
    parent.local_state_bindings.insert(effects.local_state_bindings.begin(),
                                       effects.local_state_bindings.end());
    parent.local_owned_state_scopes.insert(effects.local_owned_state_scopes.begin(),
                                           effects.local_owned_state_scopes.end());
    parent.local_captures_retained_snapshot =
        parent.local_captures_retained_snapshot || effects.local_captures_retained_snapshot;
    for (const ComponentId child : effects.direct_descendants) {
        if (std::ranges::find(parent.direct_descendants, child) == parent.direct_descendants.end())
            parent.direct_descendants.push_back(child);
    }
    add_descendants(parent.descendants, effects.descendants);
    for (const RetainedValueEffects& source : effects.retained_values) {
        auto destination =
            std::ranges::find(parent.retained_values, source.query, &RetainedValueEffects::query);
        if (destination == parent.retained_values.end()) {
            parent.retained_values.push_back(source);
            continue;
        }
        destination->values.insert(source.values.begin(), source.values.end());
    }
    for (const RetainedSequenceEffect& source : effects.retained_sequences) {
        auto destination = std::ranges::find(parent.retained_sequences, source.query,
                                             &RetainedSequenceEffect::query);
        if (destination == parent.retained_sequences.end()) {
            parent.retained_sequences.push_back(source);
            continue;
        }
        destination->sequence = source.sequence;
        destination->generation = source.generation;
    }
    if (effects.captures_retained_snapshot) {
        parent.captures_retained_snapshot = true;
        parent.retained_snapshot = effects.retained_snapshot;
    }
}

bool DescriptionBuilder::component_cache_entry_current(const ComponentCacheEntry& entry,
                                                       const std::string_view source_path,
                                                       const ComponentInputs& inputs) const {
    if (entry.source_path != source_path || !same_inputs(entry.inputs, inputs) ||
        !same_contextual_host_roots(entry.contextual_host_roots)) {
        return false;
    }
    return component_effects_current(entry.effects, entry.host_invalidation_count,
                                     entry.contextual_host_roots);
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

bool DescriptionBuilder::component_effects_current(
    const ComponentEffects& effects, const std::uint64_t host_invalidation_count,
    const std::shared_ptr<const runtime::HostRoots>& contextual_host_roots) const {
    if (!same_contextual_host_roots(contextual_host_roots))
        return false;
    if (effects.captures_retained_snapshot && effects.retained_snapshot != retained_snapshot_)
        return false;
    if (host_invalidation_count != application_.host().invalidation_count()) {
        for (const auto& [path, dependency] : effects.host_values) {
            if (!host_read_current(path, dependency))
                return false;
        }
    }
    for (const auto& [address, value] : effects.state_values) {
        const runtime::Value* const current = application_.state().find(address);
        if (current == nullptr || *current != value)
            return false;
    }
    for (const RetainedValueEffects& effect : effects.retained_values) {
        const RetainedDescriptionSnapshot::Node* const retained = retained_widget(effect.query);
        for (const auto& [name, expected] : effect.values) {
            const runtime::Value* const current =
                retained != nullptr ? retained->retained_value(name) : nullptr;
            if (current == nullptr) {
                if (expected.has_value())
                    return false;
            } else if (!expected.has_value() || *current != *expected) {
                return false;
            }
        }
    }
    for (const RetainedSequenceEffect& effect : effects.retained_sequences) {
        const RetainedDescriptionSnapshot::Node* const retained = retained_widget(effect.query);
        const std::shared_ptr<const runtime::IndexableSequence> sequence =
            retained != nullptr ? retained->virtual_sequence.lock() : nullptr;
        const std::optional<DescriptionSequenceGeneration> generation =
            retained != nullptr ? retained->virtual_sequence_generation : std::nullopt;
        if (sequence != effect.sequence.lock() || generation != effect.generation)
            return false;
    }
    return true;
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
