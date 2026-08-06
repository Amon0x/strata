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

using JsonValue = data::JsonView;
using JsonArray = data::JsonArrayView;
using JsonObject = data::JsonObjectView;

constexpr std::size_t maximum_component_cache_entries = 512U;

[[nodiscard]] bool is_executable_expression(
    const runtime::ExpressionValue& value
) noexcept {
    return value.collection() != nullptr || value.lambda() != nullptr ||
        value.action() != nullptr || value.list() != nullptr ||
        value.object() != nullptr || value.component_template() != nullptr;
}

void bind_expression(
    runtime::ExpressionScope& scope,
    std::string name,
    runtime::ExpressionValue value,
    const bool state_binding = false
) {
    scope.state_bindings.erase(name);
    if (state_binding && value.lexical_state_binding().has_value()) {
        scope.state_bindings.insert_or_assign(
            name,
            *value.lexical_state_binding()
        );
    }
    if (is_executable_expression(value)) {
        scope.values.erase(name);
        scope.executable_values.insert_or_assign(std::move(name), std::move(value));
    } else if (value.value() != nullptr) {
        scope.executable_values.erase(name);
        scope.values.insert_or_assign(std::move(name), *value.value());
    }
}

[[nodiscard]] JsonValue required(const JsonValue value, const std::string_view field) {
    const JsonValue child = value.find(field);
    if (!child) throw std::logic_error("validated portable IR lost field '" + std::string(field) + "'");
    return child;
}

[[nodiscard]] std::string_view string_field(
    const JsonValue value,
    const std::string_view field
) {
    const std::optional<std::string_view> child = required(value, field).string();
    if (!child.has_value()) throw std::logic_error("validated portable IR string field changed type");
    return *child;
}

[[nodiscard]] JsonArray array_field(const JsonValue value, const std::string_view field) {
    const std::optional<JsonArray> child = required(value, field).array();
    if (!child.has_value()) throw std::logic_error("validated portable IR array field changed type");
    return *child;
}

[[nodiscard]] JsonObject object_field(const JsonValue value, const std::string_view field) {
    const std::optional<JsonObject> child = required(value, field).object();
    if (!child.has_value()) throw std::logic_error("validated portable IR object field changed type");
    return *child;
}

[[nodiscard]] std::optional<std::string> key_from_value(const runtime::ExpressionValue& value) {
    const runtime::Value* scalar = value.value();
    if (scalar == nullptr || scalar->kind() == runtime::ValueKind::null_value) return std::nullopt;
    if (scalar->key() != nullptr) return scalar->key()->value;
    if (scalar->string() != nullptr && !scalar->string()->empty()) return *scalar->string();
    if (scalar->number() != nullptr) return runtime::display_string(*scalar);
    return std::nullopt;
}

[[nodiscard]] std::string component_instance_path(
    const std::string_view parent,
    const std::string_view type,
    const std::optional<std::string>& key,
    const std::string_view source_path
) {
    std::string parent_path(parent);
    if (key.has_value()) {
        const std::size_t separator = parent_path.rfind('/');
        if (separator != std::string::npos &&
            parent_path.substr(separator + 1U).starts_with("for:")) {
            parent_path.resize(separator);
        }
    }
    const std::string identity = key.has_value()
                                     ? "key:" + *key
                                     : "call:" + std::string(source_path);
    return parent_path + "/component:" + std::string(type) + "/" + identity;
}

[[nodiscard]] std::string repeated_item_segment(const runtime::Value& value, const std::size_t index) {
    const runtime::Value* stable = value.field("key");
    if (stable == nullptr) stable = value.field("id");
    if (stable == nullptr && value.kind() != runtime::ValueKind::list &&
        value.kind() != runtime::ValueKind::object && value.kind() != runtime::ValueKind::null_value) {
        stable = &value;
    }
    return stable != nullptr ? runtime::display_string(*stable) : std::to_string(index);
}

[[nodiscard]] std::string lazy_item_key(const runtime::Value& value, const std::size_t index) {
    const runtime::Value* stable = value.field("key");
    if (stable == nullptr) stable = value.field("id");
    if (stable != nullptr) {
        if (stable->key() != nullptr && !stable->key()->value.empty()) {
            return stable->key()->value;
        }
        if (stable->string() != nullptr && !stable->string()->empty()) {
            return *stable->string();
        }
    }
    return "dsl-lazy-" + std::to_string(index);
}

class RepeaterIndexableSequence final : public runtime::IndexableSequence {
public:
    using KeyFactory = std::function<std::string(const runtime::Value&, std::size_t)>;

    RepeaterIndexableSequence(
        const std::uint64_t generation,
        runtime::Value source,
        std::optional<std::vector<std::size_t>> selection,
        KeyFactory key_factory
    ) : generation_(generation),
        source_(std::move(source)),
        selection_(std::move(selection)),
        key_factory_(std::move(key_factory)) {
        if (source_.list() == nullptr) {
            throw std::invalid_argument("repeater sequence source must be a list");
        }
        if (!key_factory_) throw std::invalid_argument("repeater sequence requires a key evaluator");
    }

    [[nodiscard]] std::uint64_t generation() const noexcept override { return generation_; }

    [[nodiscard]] std::size_t count() const noexcept override {
        return selection_.has_value() ? selection_->size() : source_.list()->values.size();
    }

    [[nodiscard]] const runtime::Value& item_at(const std::size_t index) const override {
        return source_.list()->values.at(source_index_at(index));
    }

    [[nodiscard]] std::size_t source_index_at(const std::size_t index) const override {
        if (index >= count()) throw std::out_of_range("repeater sequence index is outside the selection");
        return selection_.has_value() ? selection_->at(index) : index;
    }

    [[nodiscard]] std::string key_at(const std::size_t index) const override {
        return key_factory_(item_at(index), source_index_at(index));
    }

    [[nodiscard]] std::optional<std::size_t> index_of_key(
        const std::string_view key
    ) const override {
        for (std::size_t index = 0U; index < count(); ++index) {
            if (key_at(index) == key) return index;
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
    void exclude(std::set<std::string, std::less<>> names) {
        excluded.merge(names);
    }

    void lexical(
        const std::string_view name,
        const runtime::ExpressionDependencyValue& value
    ) override {
        if (excluded.contains(name)) return;
        lexical_values.insert_or_assign(std::string(name), value);
    }

    void host(const runtime::ExpressionHostDependency& dependency) override {
        host_values.insert_or_assign(
            runtime::canonical_host_dependency_path(dependency.path),
            dependency
        );
    }

    std::set<std::string, std::less<>> excluded;
    std::map<std::string, runtime::ExpressionDependencyValue, std::less<>> lexical_values;
    std::map<std::string, runtime::ExpressionHostDependency, std::less<>> host_values;
};

[[nodiscard]] bool repeater_dependencies_current(
    const DescriptionSequenceGeneration& previous,
    const runtime::ExpressionScope& scope,
    const runtime::ExpressionRuntime& expressions
) {
    for (const auto& [name, value] : previous.lexical_dependencies) {
        if (!value.cacheable()) return false;
        const std::optional<runtime::ExpressionDependencyValue> current =
            runtime::expression_scope_dependency(scope, name);
        if (!current.has_value() || *current != value) return false;
    }
    for (const auto& [canonical, dependency] : previous.host_dependencies) {
        const runtime::ExpressionHostDependency current =
            expressions.read_host_dependency(dependency.path, scope);
        if (runtime::canonical_host_dependency_path(current.path) != canonical ||
            current != dependency) {
            return false;
        }
    }
    return true;
}

class ExpressionDependencyObserverRestore final {
public:
    ExpressionDependencyObserverRestore(
        runtime::ExpressionRuntime& expressions,
        runtime::ExpressionDependencyObserver* observer
    ) : expressions_(expressions),
        previous_(expressions.exchange_dependency_observer(observer)) {}

    ~ExpressionDependencyObserverRestore() {
        static_cast<void>(expressions_.exchange_dependency_observer(previous_));
    }

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

    void lexical(
        const std::string_view name,
        const runtime::ExpressionDependencyValue& value
    ) override {
        if (upstream != nullptr) upstream->lexical(name, value);
    }

    void host(const runtime::ExpressionHostDependency& dependency) override {
        observe_host(dependency);
        if (upstream != nullptr) upstream->host(dependency);
    }
};

void merge_object(
    std::map<std::string, runtime::Value, std::less<>>& target,
    const runtime::Value& value
) {
    if (value.object() == nullptr) return;
    for (const auto& [name, property] : value.object()->fields) {
        if (name != "$bases") target.insert_or_assign(name, property);
    }
}

[[nodiscard]] runtime::Value map_value(
    std::map<std::string, runtime::Value, std::less<>> values
) {
    std::vector<std::pair<std::string, runtime::Value>> fields;
    fields.reserve(values.size());
    for (auto& [name, value] : values) fields.emplace_back(std::move(name), std::move(value));
    return runtime::Value(std::move(fields));
}

void set_layout_field(
    DescriptionNode::Properties& properties,
    std::string name,
    runtime::Value value
) {
    std::map<std::string, runtime::Value, std::less<>> fields;
    if (const auto current = properties.find("$layout");
        current != properties.end() && current->second.value() != nullptr &&
        current->second.value()->object() != nullptr) {
        for (const auto& [field_name, field_value] : current->second.value()->object()->fields) {
            fields.insert_or_assign(field_name, field_value);
        }
    }
    fields.insert_or_assign(std::move(name), std::move(value));
    properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(map_value(std::move(fields)))
    );
}

void collect_slot_names(
    const std::shared_ptr<const DescriptionNode>& node,
    std::set<std::string, std::less<>>& names
) {
    if (node->type == "Slot") {
        const auto property = node->properties.find("name");
        const runtime::Value* value = property != node->properties.end()
                                          ? property->second.value()
                                          : nullptr;
        if (value != nullptr && value->string() != nullptr && !value->string()->empty()) {
            names.insert(*value->string());
        }
    }
    for (std::size_t index = 0U; index < node->children->size(); ++index) {
        collect_slot_names(node->children->at(index), names);
    }
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> project_slots(
    const std::shared_ptr<const DescriptionNode>& node,
    const std::map<std::string, std::vector<std::shared_ptr<const DescriptionNode>>, std::less<>>& projected
) {
    if (node->type == "Slot") {
        const auto property = node->properties.find("name");
        const runtime::Value* value = property != node->properties.end()
                                          ? property->second.value()
                                          : nullptr;
        if (value != nullptr && value->string() != nullptr) {
            const auto replacement = projected.find(*value->string());
            if (replacement != projected.end()) {
                auto resolved = std::make_shared<DescriptionNode>(*node);
                resolved->children = std::make_shared<const EagerDescriptionChildren>(
                    replacement->second
                );
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
    if (!changed) return node;
    auto resolved = std::make_shared<DescriptionNode>(*node);
    resolved->children = std::make_shared<const EagerDescriptionChildren>(std::move(children));
    return resolved;
}

} // namespace

struct DescriptionBuilder::RepeaterIdentityEvaluationState final {
    RepeaterIdentityEvaluationState(
        runtime::ApplicationContext& application,
        const WidgetRegistry& source_widgets,
        Scope source_scope,
        std::string source_item_name,
        std::optional<std::string> source_index_name,
        data::JsonView source_identity
    ) : unit_keep_alive(application.active_unit()),
        widgets(source_widgets),
        evaluator(std::make_unique<DescriptionBuilder>(application, widgets)),
        scope(std::move(source_scope)),
        item_name(std::move(source_item_name)),
        index_name(std::move(source_index_name)),
        identity(std::move(source_identity)) {
        evaluator->set_contextual_host_roots(scope.expressions.contextual_host_roots);
    }

    [[nodiscard]] std::string key(
        const runtime::Value& item,
        const std::size_t source_index
    ) {
        std::scoped_lock lock(mutex);
        Scope item_scope = scope;
        item_scope.expressions.values.insert_or_assign(item_name, item);
        if (index_name.has_value()) {
            item_scope.expressions.values.insert_or_assign(
                *index_name,
                runtime::Value(static_cast<double>(source_index))
            );
        }
        // The complete domain was validated while the sequence generation was constructed.
        // Later observed-key queries are deterministic reads and do not leak counters or diagnostics
        // into an unrelated row materialization transaction.
        evaluator->diagnostics_.clear();
        evaluator->evaluated_expressions_ = 0U;
        return evaluator->evaluate_repeater_identity(identity, item_scope);
    }

    std::shared_ptr<const runtime::RuntimeUnit> unit_keep_alive;
    WidgetRegistry widgets;
    std::unique_ptr<DescriptionBuilder> evaluator;
    Scope scope;
    std::string item_name;
    std::optional<std::string> index_name;
    data::JsonView identity;
    std::mutex mutex;
};

struct DescriptionBuilder::GeneratedRowEvaluationContext final {
    GeneratedRowEvaluationContext(
        runtime::ApplicationContext& application,
        const WidgetRegistry& source_widgets,
        std::map<std::string, runtime::Value, std::less<>> contextual_host_roots,
        std::shared_ptr<const RetainedDescriptionSnapshot> retained
    ) : widgets(source_widgets),
        evaluator(std::make_unique<DescriptionBuilder>(application, widgets)) {
        evaluator->set_contextual_host_roots(std::move(contextual_host_roots));
        evaluator->retained_snapshot_ = std::move(retained);
    }

    void begin() {
        evaluator->diagnostics_.clear();
        evaluator->evaluated_expressions_ = 0U;
        evaluator->described_nodes_ = 0U;
        evaluator->current_layer_state_scopes_.clear();
        evaluator->resolved_styles_.clear();
    }

    [[nodiscard]] std::shared_ptr<const DescriptionNode> finish(
        std::shared_ptr<const DescriptionNode> node,
        const std::size_t synthesized_nodes,
        std::string materialization_key
    ) {
        if (node == nullptr) {
            throw std::logic_error("generated row evaluator returned a null description");
        }
        auto anchored = std::make_shared<DescriptionNode>(*node);
        if (!anchored->materialization_key.has_value()) {
            anchored->materialization_key = std::move(materialization_key);
        }
        DescriptionMaterialization result{
            std::move(evaluator->current_layer_state_scopes_),
            std::move(evaluator->diagnostics_),
            evaluator->evaluated_expressions_,
            evaluator->described_nodes_,
        };
        if (synthesized_nodes > std::numeric_limits<std::size_t>::max() -
                                    result.described_nodes) {
            throw std::overflow_error("generated row described-node count exhausted");
        }
        result.described_nodes += synthesized_nodes;
        if (anchored->materialization_result != nullptr) {
            const DescriptionMaterialization& nested = *anchored->materialization_result;
            result.owned_state_scopes.insert(
                nested.owned_state_scopes.begin(), nested.owned_state_scopes.end()
            );
            result.diagnostics.insert(
                result.diagnostics.end(), nested.diagnostics.begin(), nested.diagnostics.end()
            );
            if (nested.evaluated_expressions > std::numeric_limits<std::size_t>::max() -
                                                    result.evaluated_expressions ||
                nested.described_nodes > std::numeric_limits<std::size_t>::max() -
                                             result.described_nodes) {
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
    LazyRowEvaluationState(
        runtime::ApplicationContext& application,
        const WidgetRegistry& source_widgets,
        Scope source_scope,
        std::string source_item_name,
        std::optional<std::string> source_index_name,
        data::JsonView source_block,
        std::shared_ptr<const RetainedDescriptionSnapshot> retained
    ) : unit_keep_alive(application.active_unit()),
        evaluation(
            application,
            source_widgets,
            source_scope.expressions.contextual_host_roots,
            std::move(retained)
        ),
        scope(std::move(source_scope)),
        item_name(std::move(source_item_name)),
        index_name(std::move(source_index_name)),
        block(std::move(source_block)) {}

    [[nodiscard]] std::shared_ptr<const DescriptionNode> materialize(
        const runtime::IndexableSequence& sequence,
        const std::size_t lazy_index
    ) {
        evaluation.begin();
        const runtime::Value& item = sequence.item_at(lazy_index);
        const std::size_t source_index = sequence.source_index_at(lazy_index);
        const std::string canonical_key = sequence.key_at(lazy_index);
        Scope item_scope = scope;
        item_scope.expressions.values.insert_or_assign(item_name, item);
        if (index_name.has_value()) {
            item_scope.expressions.values.insert_or_assign(
                *index_name,
                runtime::Value(static_cast<double>(source_index))
            );
        }
        item_scope.instance_path += "/" + item_name + ":" + canonical_key;
        item_scope.runtime_state_scope = item_scope.instance_path;
        item_scope.expressions.component_path = item_scope.instance_path;
        evaluation.evaluator->bind_scope_state(item_scope);
        std::vector<std::shared_ptr<const DescriptionNode>> nodes =
            evaluation.evaluator->build_block(
            block,
            std::move(item_scope)
        );
        if (nodes.size() != 1U) {
            throw std::logic_error(
                "validated Repeater identity selected a row body that did not produce one root"
            );
        }
        auto anchored = std::make_shared<DescriptionNode>(*nodes.front());
        anchored->key = canonical_key;
        return evaluation.finish(
            std::move(anchored),
            0U,
            canonical_key
        );
    }

    std::shared_ptr<const runtime::RuntimeUnit> unit_keep_alive;
    GeneratedRowEvaluationContext evaluation;
    Scope scope;
    std::string item_name;
    std::optional<std::string> index_name;
    data::JsonView block;
};

struct DescriptionBuilder::WidgetRowEvaluationState final {
    WidgetRowEvaluationState(
        runtime::ApplicationContext& application,
        const WidgetRegistry& source_widgets,
        Scope source_scope,
        WidgetGeneratedChildHook source_factory,
        std::shared_ptr<const RetainedDescriptionSnapshot> retained
    ) : evaluation(
            application,
            source_widgets,
            source_scope.expressions.contextual_host_roots,
            std::move(retained)
        ),
        caller(std::move(source_scope)),
        factory(std::move(source_factory)),
        actions(&application.bundle()->action_registry()) {}

    [[nodiscard]] std::shared_ptr<const DescriptionNode> materialize(
        const std::size_t index
    ) {
        evaluation.begin();
        WidgetDescriptionExpansion item;
        WidgetDescriptionScope item_scope(
            item,
            caller.runtime_state_scope,
            *actions,
            evaluation.widgets,
            nullptr,
            [this](
                const std::string_view component,
                std::string key,
                WidgetTemplateArguments arguments
            ) {
                return evaluation.evaluator->build_component_template(
                    component,
                    std::move(key),
                    std::move(arguments),
                    caller
                );
            },
            {}
        );
        std::shared_ptr<const DescriptionNode> row = factory(item_scope, index);
        const std::string materialization_key = row != nullptr && row->key.has_value()
            ? *row->key
            : "widget-generated-" + std::to_string(index);
        return evaluation.finish(
            std::move(row),
            item.synthesized_nodes,
            materialization_key
        );
    }

    GeneratedRowEvaluationContext evaluation;
    Scope caller;
    WidgetGeneratedChildHook factory;
    const runtime::RuntimeActionRegistry* actions;
};

DescriptionBuilder::DescriptionBuilder(runtime::ApplicationContext& application)
    : application_(application),
      owned_widgets_(std::make_unique<WidgetRegistry>()),
      widgets_(*owned_widgets_),
      expressions_(std::make_unique<runtime::ExpressionRuntime>(
          application.host(),
          application.bundle()->action_registry()
      )) {}

DescriptionBuilder::DescriptionBuilder(
    runtime::ApplicationContext& application,
    const WidgetRegistry& widgets
)
    : application_(application),
      owned_widgets_(),
      widgets_(widgets),
      expressions_(std::make_unique<runtime::ExpressionRuntime>(
          application.host(),
          application.bundle()->action_registry()
      )) {}

DescriptionBuildResult DescriptionBuilder::build(
    const runtime::LayerRole role,
    const std::string_view name
) {
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

bool DescriptionBuilder::observes_retained_value(
    const RetainedNode& node,
    const std::string_view name,
    const bool include_lazy_snapshots
) const {
    if (retained_snapshot_ == nullptr || layer_cache_.empty()) return true;
    const auto effects_observe =
        [this, &node, name, include_lazy_snapshots](const ComponentEffects& effects) {
        if (include_lazy_snapshots && effects.captures_retained_snapshot) {
            return true;
        }
        return std::ranges::any_of(
            effects.retained_values,
            [this, &node, name](const RetainedValueEffects& effect) {
                if (!effect.values.contains(name)) return false;
                const RetainedDescriptionSnapshot::Node* const retained =
                    retained_widget(effect.query);
                return retained != nullptr && retained->identity == node.identity();
            }
        );
    };
    if (std::ranges::any_of(
            layer_cache_,
            [&effects_observe](const auto& entry) {
                return effects_observe(entry.second.effects);
            }
        )) {
        return true;
    }
    return std::ranges::any_of(
        component_cache_,
        [&effects_observe](const auto& entry) {
            return effects_observe(entry.second.effects);
        }
    );
}

void DescriptionBuilder::set_contextual_host_roots(
    std::map<std::string, runtime::Value, std::less<>> roots
) {
    contextual_host_roots_ = std::move(roots);
}

DescriptionLayersBuildResult DescriptionBuilder::build_layers(
    const std::span<const LayerDescriptionRequest> layers
) {
    if (layers.empty()) throw std::invalid_argument("description layer list must not be empty");
    if (component_cache_unit_ != application_.active_unit() ||
        component_cache_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        component_cache_.clear();
        layer_cache_.clear();
        component_cache_epoch_ = 0U;
        component_cache_unit_ = application_.active_unit();
    }
    ++component_cache_epoch_;
    visited_component_cache_keys_.clear();
    diagnostics_.clear();
    evaluated_expressions_ = 0U;
    described_nodes_ = 0U;
    resolved_styles_.clear();
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
        std::vector<std::pair<std::uint64_t, std::string>> eviction_candidates;
        eviction_candidates.reserve(component_cache_.size());
        for (const auto& [key, entry] : component_cache_) {
            if (!visited_component_cache_keys_.contains(key)) {
                eviction_candidates.emplace_back(entry.last_used_epoch, key);
            }
        }
        std::ranges::sort(eviction_candidates);
        const std::size_t removal_count = std::min(
            component_cache_.size() - maximum_component_cache_entries,
            eviction_candidates.size()
        );
        for (std::size_t index = 0U; index < removal_count; ++index) {
            component_cache_.erase(eviction_candidates[index].second);
        }
    }
    return DescriptionLayersBuildResult{
        std::move(roots),
        std::move(layer_state_scopes),
        std::move(diagnostics_),
        evaluated_expressions_,
        described_nodes_,
    };
}

std::shared_ptr<const DescriptionNode> DescriptionBuilder::build_layer(
    const runtime::LayerRole role,
    const std::string_view name
) {
    const std::shared_ptr<const runtime::RuntimeUnit>& unit = application_.active_unit();
    if (unit == nullptr) throw std::logic_error("description build requires an active runtime unit");
    const JsonValue declaration = role == runtime::LayerRole::screen
        ? unit->screen(name)
        : unit->overlay(name);
    if (!declaration) throw std::invalid_argument("requested layer declaration is not active");
    const std::string declaration_scope =
        std::string(role == runtime::LayerRole::screen ? "screen " : "overlay ") + std::string(name);
    const std::string source_path(string_field(declaration, "path"));
    const std::string cache_key =
        std::string(role == runtime::LayerRole::screen ? "screen\n" : "overlay\n") +
        std::string(name);
    std::shared_ptr<const DescriptionNode> patched_layer_root;
    if (auto cached = layer_cache_.find(cache_key);
        cached != layer_cache_.end() &&
        cached->second.role == role &&
        cached->second.name == name &&
        cached->second.source_path == source_path &&
        cached->second.contextual_host_roots == contextual_host_roots_) {
        std::map<const DescriptionNode*, std::shared_ptr<const DescriptionNode>> replacements;
        bool valid = true;
        for (const std::string& child_key :
             cached->second.effects.direct_descendant_cache_keys) {
            const auto child_before = component_cache_.find(child_key);
            if (child_before == component_cache_.end()) {
                valid = false;
                break;
            }
            const std::shared_ptr<const DescriptionNode> previous =
                child_before->second.root;
            if (refresh_component_cache_entry(child_key) ==
                ComponentRefreshResult::invalid) {
                valid = false;
                break;
            }
            const auto child_after = component_cache_.find(child_key);
            if (child_after == component_cache_.end()) {
                valid = false;
                break;
            }
            if (child_after->second.root != previous) {
                replacements.insert_or_assign(
                    previous.get(),
                    child_after->second.root
                );
            }
        }
        if (valid) {
            const bool direct_current = component_effects_current(
                    cached->second.effects,
                    cached->second.host_invalidation_count,
                    cached->second.contextual_host_roots
                );
            if (replacements.empty() && direct_current) {
                cached->second.host_invalidation_count =
                    application_.host().invalidation_count();
                replay_component_effects(cached->second.effects);
                return cached->second.root;
            }
            if (direct_current) {
                patched_layer_root = replace_component_subtrees(
                    cached->second.root,
                    replacements
                );
            }
        }
    }

    Scope scope{
        runtime::ExpressionScope{
            .values = {},
            .executable_values = {},
            .contextual_host_roots = contextual_host_roots_,
            .component_path = declaration_scope,
            .state_bindings = {},
            .host_dependency_overrides = {},
            .lexical_dependency_overrides = {},
        },
        declaration_scope,
        declaration_scope,
        declaration_scope,
        {},
    };
    ComponentEffects effects;
    component_effect_stack_.push_back(&effects);
    ComponentExpressionDependencies dependencies;
    dependencies.observe_host = [this](
        const runtime::ExpressionHostDependency& dependency
    ) {
        observe_host_dependency(dependency);
    };
    ExpressionDependencyObserverRestore dependency_observer(
        *expressions_,
        &dependencies
    );
    dependencies.upstream = dependency_observer.previous();
    std::vector<std::shared_ptr<const DescriptionNode>> roots;
    try {
        roots = build_block(required(declaration, "body"), std::move(scope));
    } catch (...) {
        component_effect_stack_.pop_back();
        throw;
    }
    component_effect_stack_.pop_back();
    auto declaration_children = std::make_shared<const EagerDescriptionChildren>(std::move(roots));
    auto root = DescriptionNode::create(
        role == runtime::LayerRole::screen ? "$screen" : "$overlay",
        std::nullopt,
        source_path,
        declaration_scope,
        {},
        std::move(declaration_children)
    );
    ++described_nodes_;
    if (patched_layer_root != nullptr) root = std::move(patched_layer_root);
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

std::vector<std::shared_ptr<const DescriptionNode>> DescriptionBuilder::build_block(
    const JsonValue block,
    Scope scope,
    const std::span<const std::size_t> skipped_statement_indices
) {
    std::vector<std::shared_ptr<const DescriptionNode>> nodes;
    const JsonArray statements = array_field(block, "statements");
    for (std::size_t statement_index = 0U; statement_index < statements.size(); ++statement_index) {
        if (std::ranges::binary_search(skipped_statement_indices, statement_index)) continue;
        const JsonValue statement = statements[statement_index];
        const std::string_view kind = string_field(statement, "kind");
        if (kind == "state") {
            const std::string name(string_field(statement, "name"));
            const runtime::UnitStateDeclaration* declaration =
                application_.active_unit()->state_declaration(scope.declaration_scope, name);
            if (declaration == nullptr) throw std::logic_error("indexed state declaration is missing");
            const std::string address_scope =
                "dsl:" + application_.active_unit()->source_id() + ":" + scope.instance_path +
                "/state:" + declaration->declaration_path;
            own_state_scope(address_scope);
            scope.expressions.state_bindings.insert_or_assign(
                name,
                runtime::LexicalStateBinding{
                    runtime::StateAddress{address_scope, name},
                    scope.declaration_scope,
                }
            );
            bind_state_scope(
                scope.runtime_state_scope,
                name,
                scope.declaration_scope,
                address_scope
            );
            const runtime::ExpressionValue evaluated = evaluate(required(statement, "initializer"), scope.expressions);
            const runtime::Value initial = require_value(evaluated, statement);
            const std::string slot_type = declaration->type_id == "dsl.unknown"
                                              ? std::string(initial.state_type_id())
                                              : declaration->type_id;
            const runtime::StateSlot slot{
                name,
                slot_type,
                initial,
                scope.declaration_scope,
            };
            const runtime::StateAddress address{address_scope, name};
            if (declaration->persistence_key.has_value() &&
                application_.state().find(address) == nullptr) {
                if (const runtime::Value* persisted = application_.durability().application_value(
                        *declaration->persistence_key
                    ); persisted != nullptr) {
                    if (declaration->schema != nullptr && declaration->schema->accepts(*persisted)) {
                        static_cast<void>(application_.state().write(address, slot, *persisted));
                    } else {
                        application_.services().report(runtime::RuntimeDiagnostic{
                            "STRATA.DURABILITY.TYPE_MISMATCH",
                            "Persisted value '" + *declaration->persistence_key +
                                "' no longer matches state '" + name + "' and was discarded.",
                            scope.runtime_state_scope,
                            slot.type_id,
                            runtime::DiagnosticSeverity::warning,
                            std::nullopt,
                        });
                        static_cast<void>(application_.durability().erase_application_value(
                            *declaration->persistence_key
                        ));
                    }
                }
            }
            const runtime::Value& state = application_.state().read(address, slot);
            observe_state_value(runtime::StateAddress{address_scope, name}, state);
            scope.expressions.values.insert_or_assign(name, state);
            continue;
        }
        if (kind == "derived") {
            const std::string name(string_field(statement, "name"));
            bind_expression(
                scope.expressions,
                name,
                evaluate(required(statement, "expression"), scope.expressions)
            );
            continue;
        }
        if (kind == "node") {
            nodes.push_back(build_call(required(statement, "call"), scope));
            continue;
        }
        if (kind == "if") {
            const runtime::Value condition = require_value(
                evaluate(required(statement, "condition"), scope.expressions),
                statement
            );
            const JsonValue selected = required(statement, runtime::truthy(condition) ? "then" : "else");
            if (!selected.is_null()) {
                auto branch = build_block(selected, scope);
                nodes.insert(nodes.end(), branch.begin(), branch.end());
            }
            continue;
        }
        if (kind == "when") {
            const runtime::Value subject = require_value(
                evaluate(required(statement, "subject"), scope.expressions),
                statement
            );
            for (const JsonValue branch : array_field(statement, "branches")) {
                const JsonValue match = required(branch, "match");
                if (!match.is_null()) {
                    const runtime::Value candidate = require_value(evaluate(match, scope.expressions), match);
                    if (candidate != subject) continue;
                }
                auto selected = build_block(required(branch, "block"), scope);
                nodes.insert(nodes.end(), selected.begin(), selected.end());
                break;
            }
            continue;
        }
        if (kind == "for") {
            const runtime::ExpressionValue collection =
                evaluate(required(statement, "collection"), scope.expressions);
            const runtime::Value* scalar = collection.value();
            const runtime::ValueList* values = scalar != nullptr ? scalar->list() : nullptr;
            if (values == nullptr && collection.collection() != nullptr) {
                values = (*collection.collection())->items.list();
            }
            if (values == nullptr) continue;
            std::map<std::string, std::size_t, std::less<>> repeated_segments;
            for (std::size_t index = 0U; index < values->values.size(); ++index) {
                Scope item_scope = scope;
                const std::string item_name(string_field(statement, "itemName"));
                item_scope.expressions.values.insert_or_assign(item_name, values->values[index]);
                const JsonValue index_name = required(statement, "indexName");
                if (const std::optional<std::string_view> encoded_index = index_name.string();
                    encoded_index.has_value()) {
                    item_scope.expressions.values.insert_or_assign(
                        std::string(*encoded_index),
                        runtime::Value(static_cast<double>(index))
                    );
                }
                const JsonValue filter = required(statement, "filter");
                if (!filter.is_null() &&
                    !runtime::truthy(require_value(evaluate(filter, item_scope.expressions), filter))) {
                    continue;
                }
                std::string segment = repeated_item_segment(values->values[index], index);
                const std::size_t occurrence = repeated_segments[segment]++;
                if (occurrence != 0U) segment += ":" + std::to_string(occurrence);
                item_scope.instance_path += "/" + item_name + ":" + segment;
                item_scope.runtime_state_scope = item_scope.instance_path;
                item_scope.expressions.component_path = item_scope.instance_path;
                bind_scope_state(item_scope);
                auto iteration = build_block(required(statement, "block"), std::move(item_scope));
                if (!iteration.empty()) {
                    auto anchored = std::make_shared<DescriptionNode>(*iteration.front());
                    anchored->materialization_key = lazy_item_key(values->values[index], index);
                    iteration.front() = std::move(anchored);
                }
                nodes.insert(nodes.end(), iteration.begin(), iteration.end());
            }
            continue;
        }
        throw std::logic_error("validated portable IR contains an unknown statement kind");
    }
    return nodes;
}

std::string DescriptionBuilder::evaluate_repeater_identity(
    const JsonValue identity,
    const Scope& scope
) {
    const std::string_view kind = string_field(identity, "kind");
    if (kind == "key") {
        const JsonValue expression = required(identity, "expression");
        const runtime::ExpressionValue value = evaluate(expression, scope.expressions);
        const std::optional<std::string> key = key_from_value(value);
        if (!key.has_value() || key->empty()) {
            throw std::invalid_argument(
                "Repeater root key must resolve to a non-empty string, key, or finite number"
            );
        }
        return *key;
    }
    if (kind == "block") {
        std::optional<std::string> result;
        for (const JsonValue statement : array_field(identity, "statements")) {
            const std::string candidate = evaluate_repeater_identity(statement, scope);
            if (candidate.empty()) continue;
            if (result.has_value()) {
                throw std::logic_error("validated Repeater identity produced multiple root keys");
            }
            result = candidate;
        }
        if (!result.has_value()) {
            throw std::logic_error("validated Repeater identity did not select a root key");
        }
        return *result;
    }
    if (kind == "if") {
        const JsonValue condition = required(identity, "condition");
        const runtime::Value selected = require_value(
            evaluate(condition, scope.expressions),
            condition
        );
        return evaluate_repeater_identity(
            required(identity, runtime::truthy(selected) ? "then" : "else"),
            scope
        );
    }
    if (kind == "when") {
        const JsonValue subject_expression = required(identity, "subject");
        const runtime::Value subject = require_value(
            evaluate(subject_expression, scope.expressions),
            subject_expression
        );
        for (const JsonValue branch : array_field(identity, "branches")) {
            const JsonValue match = required(branch, "match");
            if (!match.is_null()) {
                const runtime::Value candidate = require_value(
                    evaluate(match, scope.expressions),
                    match
                );
                if (candidate != subject) continue;
            }
            return evaluate_repeater_identity(required(branch, "identity"), scope);
        }
        throw std::logic_error("validated Repeater identity when did not select a branch");
    }
    throw std::logic_error("validated Repeater identity contains an unknown extractor kind");
}

const RetainedDescriptionSnapshot::Node* DescriptionBuilder::retained_widget(
    const RetainedQuery& query
) const noexcept {
    if (retained_snapshot_ == nullptr) return nullptr;
    if (query.key.has_value()) {
        return retained_snapshot_->find_key(
            *query.key,
            query.source_path,
            query.state_scope,
            query.type
        );
    }
    const std::vector<const RetainedDescriptionSnapshot::Node*>* candidates =
        retained_snapshot_->find_source(query.source_path);
    if (candidates == nullptr) return nullptr;
    const auto found = std::ranges::find_if(*candidates, [&](const auto* candidate) {
        const bool compatible_type = candidate->type == query.type ||
            (query.type == "Repeater" && candidate->type == "VirtualList");
        return compatible_type && candidate->state_scope == query.state_scope;
    });
    return found != candidates->end() ? *found : nullptr;
}

DescriptionBuilder::RepeaterChildren DescriptionBuilder::build_repeater_children(
    const JsonValue block,
    Scope scope,
    const RetainedDescriptionSnapshot::Node* const retained_widget
) {
    const JsonArray statements = array_field(block, "statements");
    if (statements.size() != 1U || string_field(statements.front(), "kind") != "for") {
        return {};
    }
    const JsonValue statement = statements.front();
    const std::string item_name(string_field(statement, "itemName"));
    const JsonValue index_name = required(statement, "indexName");
    const std::optional<std::string> index_name_value = [&] {
        const std::optional<std::string_view> encoded = index_name.string();
        return encoded.has_value()
            ? std::optional<std::string>(std::string(*encoded))
            : std::nullopt;
    }();
    const JsonValue filter = required(statement, "filter");
    const JsonValue identity = required(statement, "identity");
    if (identity.is_null()) {
        throw std::logic_error("validated Repeater loop lost its root-key extractor");
    }

    std::set<std::string, std::less<>> filter_bindings{item_name};
    if (index_name_value.has_value()) filter_bindings.insert(*index_name_value);

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
        ExpressionDependencyObserverRestore dependency_observer(
            *expressions_,
            &dependencies
        );
        const runtime::ExpressionValue collection = evaluate(
            required(statement, "collection"),
            scope.expressions
        );
        const runtime::Value* scalar = collection.value();
        const runtime::ValueList* values = scalar != nullptr ? scalar->list() : nullptr;
        if (values == nullptr && collection.collection() != nullptr) {
            values = (*collection.collection())->items.list();
        }
        if (values == nullptr) return {};
        const runtime::Value source_items =
            scalar != nullptr ? *scalar : (*collection.collection())->items;
        stamp.source = runtime::capture_expression_dependency(collection);

        if (previous_sequence != nullptr && previous_stamp != nullptr &&
            stamp.source.cacheable() &&
            previous_stamp->active_unit == stamp.active_unit &&
            previous_stamp->source == stamp.source &&
            repeater_dependencies_current(
                *previous_stamp,
                scope.expressions,
                *expressions_
            )) {
            stamp = *previous_stamp;
            sequence = previous_sequence;
        } else {
            dependencies.exclude(std::move(filter_bindings));
            std::optional<std::vector<std::size_t>> selection;
            if (!filter.is_null()) {
                selection.emplace();
                selection->reserve(values->values.size());
            }
            std::map<std::string, std::size_t, std::less<>> first_source_index_by_key;
            for (std::size_t index = 0U; index < values->values.size(); ++index) {
                Scope item_scope = scope;
                item_scope.expressions.values.insert_or_assign(item_name, values->values[index]);
                if (index_name_value.has_value()) {
                    item_scope.expressions.values.insert_or_assign(
                        *index_name_value,
                        runtime::Value(static_cast<double>(index))
                    );
                }
                if (!filter.is_null() && !runtime::truthy(require_value(
                        evaluate(filter, item_scope.expressions),
                        filter
                    ))) {
                    continue;
                }
                const std::string canonical_key = evaluate_repeater_identity(identity, item_scope);
                const auto [duplicate, inserted] = first_source_index_by_key.emplace(
                    canonical_key,
                    index
                );
                if (!inserted) {
                    throw std::invalid_argument(
                        "Repeater root key '" + canonical_key +
                        "' is duplicated at source indexes " +
                        std::to_string(duplicate->second) + " and " +
                        std::to_string(index)
                    );
                }
                if (selection.has_value()) selection->push_back(index);
            }
            stamp.lexical_dependencies = std::move(dependencies.lexical_values);
            stamp.host_dependencies = std::move(dependencies.host_values);
            const std::uint64_t previous_generation =
                previous_sequence != nullptr
                    ? previous_sequence->generation()
                    : 0U;
            if (previous_generation == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("repeater sequence generation exhausted");
            }
            Scope identity_scope = scope;
            identity_scope.expressions.host_dependency_overrides = stamp.host_dependencies;
            identity_scope.expressions.lexical_dependency_overrides =
                stamp.lexical_dependencies;
            // A source may read an outer binding shadowed by the loop declaration. Retain that
            // source stamp for reuse checks, but never let it override the per-item key domain.
            identity_scope.expressions.lexical_dependency_overrides.erase(item_name);
            if (index_name_value.has_value()) {
                identity_scope.expressions.lexical_dependency_overrides.erase(
                    *index_name_value
                );
            }
            auto identity_evaluation = std::make_shared<RepeaterIdentityEvaluationState>(
                application_,
                widgets_,
                std::move(identity_scope),
                item_name,
                index_name_value,
                identity
            );
            sequence = std::make_shared<const RepeaterIndexableSequence>(
                previous_generation + 1U,
                source_items,
                std::move(selection),
                [identity_evaluation = std::move(identity_evaluation)](
                    const runtime::Value& item,
                    const std::size_t source_index
                ) {
                    return identity_evaluation->key(item, source_index);
                }
            );
        }
    }

    const JsonValue item_block = required(statement, "block");
    capture_retained_snapshot();
    auto evaluation = std::make_shared<LazyRowEvaluationState>(
        application_,
        widgets_,
        scope,
        item_name,
        index_name_value,
        item_block,
        retained_snapshot_
    );
    auto source = std::make_shared<const GeneratedDescriptionChildren>(
        sequence->count(),
        [evaluation = std::move(evaluation), sequence](const std::size_t lazy_index) {
            return evaluation->materialize(*sequence, lazy_index);
        }
    );
    return RepeaterChildren{std::move(source), std::move(sequence), std::move(stamp)};
}

std::shared_ptr<const DescriptionNode> DescriptionBuilder::build_call(
    const JsonValue call,
    Scope scope
) {
    DescriptionNode::Properties properties;
    std::vector<DescriptionBehavior> behaviors;
    for (const auto& [name, expression] : object_field(call, "arguments")) {
        if (name == "behaviors") {
            behaviors = build_behaviors(expression, scope.expressions);
            continue;
        }
        properties.emplace(name, evaluate(expression, scope.expressions));
    }
    std::string type(string_field(call, "name"));
    const std::string source_path(string_field(call, "path"));
    const std::string_view call_kind = string_field(call, "kind");
    if (call_kind == "widget") {
        const auto defaults = scope.widget_defaults.find(type);
        if (defaults != scope.widget_defaults.end()) {
            if (!properties.contains("style") && defaults->second.style.has_value()) {
                properties.emplace("style", *defaults->second.style);
            }
            if (!properties.contains("variant") && defaults->second.variant.has_value()) {
                properties.emplace("variant", *defaults->second.variant);
            }
        }
    }
    normalize_layout(properties, scope.expressions);
    const auto key_property = properties.find("key");
    std::optional<std::string> key = key_property != properties.end()
                                         ? key_from_value(key_property->second)
                                         : std::nullopt;
    if (!key.has_value()) {
        const WidgetLifecycle* lifecycle = widgets_.find(type);
        if (lifecycle != nullptr && !lifecycle->describe.implicit_key_prefix.empty()) {
            key = lifecycle->describe.implicit_key_prefix + source_path;
        }
    }
    const WidgetLifecycle* const widget = widgets_.find(type);
    const std::string retained_type =
        widget != nullptr && !widget->describe.canonical_type.empty()
            ? widget->describe.canonical_type
            : type;
    const RetainedQuery retained_query{
        key,
        source_path,
        scope.runtime_state_scope,
        retained_type,
    };
    const RetainedDescriptionSnapshot::Node* retained = retained_widget(retained_query);
    RetainedDescriptionSnapshot::Node durable_retained;
    if (retained == nullptr) {
        const auto persistence_property = properties.find("persistenceKey");
        const runtime::Value* persistence_value =
            persistence_property != properties.end() ? persistence_property->second.value() : nullptr;
        const std::string* persistence_key =
            persistence_value != nullptr ? persistence_value->string() : nullptr;
        const WidgetLifecycle* lifecycle = widgets_.find(type);
        if (persistence_key != nullptr && !persistence_key->empty() && lifecycle != nullptr) {
            for (const std::string& field : lifecycle->persistence.retained_fields) {
                if (const runtime::Value* value = application_.durability().widget_value(
                        *persistence_key, field
                    ); value != nullptr) {
                    if (lifecycle->persistence.accepts == nullptr ||
                        lifecycle->persistence.accepts(field, *value)) {
                        durable_retained.retained_values.emplace(field, *value);
                    } else {
                        application_.services().report(runtime::RuntimeDiagnostic{
                            "STRATA.DURABILITY.TYPE_MISMATCH",
                            "Persisted widget field '" + field + "' for '" +
                                *persistence_key + "' has an invalid value and was discarded.",
                            scope.runtime_state_scope,
                            type,
                            runtime::DiagnosticSeverity::warning,
                            std::nullopt,
                        });
                        static_cast<void>(application_.durability().erase_widget_value(
                            *persistence_key, field
                        ));
                    }
                }
            }
            if (!durable_retained.retained_values.empty()) {
                durable_retained.type = type;
                durable_retained.key = key;
                durable_retained.source_path = source_path;
                durable_retained.state_scope = scope.runtime_state_scope;
                retained = &durable_retained;
            }
        }
    }
    RepeaterChildren repeater_children;
    const JsonValue children_value = required(call, "children");
    if (type == "Repeater" && !children_value.is_null()) {
        observe_retained_sequence(retained_query, retained);
        repeater_children = build_repeater_children(children_value, scope, retained);
    }
    widgets_.apply_layout_defaults(type, properties);
    if (call_kind == "component") {
        const JsonValue component = application_.active_unit()->component(type);
        if (!component) throw std::logic_error("component call lost its indexed declaration");

        Scope projection_scope = scope;
        for (const JsonValue entry : array_field(component, "widgetDefaults")) {
            Scope::WidgetDefault defaults;
            const JsonValue style = required(entry, "style");
            const JsonValue variant = required(entry, "variant");
            if (!style.is_null()) defaults.style = evaluate(style, scope.expressions);
            if (!variant.is_null()) defaults.variant = evaluate(variant, scope.expressions);
            projection_scope.widget_defaults.insert_or_assign(
                std::string(string_field(entry, "name")),
                std::move(defaults)
            );
        }

        std::map<
            std::string,
            std::vector<std::shared_ptr<const DescriptionNode>>,
            std::less<>
        > projected_content;
        std::vector<std::size_t> projected_statement_indices;
        std::size_t call_statement_count = 0U;
        const JsonValue call_children = required(call, "children");
        if (!call_children.is_null()) {
            const JsonArray call_statements = array_field(call_children, "statements");
            call_statement_count = call_statements.size();
            for (std::size_t statement_index = 0U;
                 statement_index < call_statements.size();
                 ++statement_index) {
                const JsonValue statement = call_statements[statement_index];
                JsonValue fill_call;
                if (string_field(statement, "kind") == "node") {
                    const JsonValue candidate = required(statement, "call");
                    if (string_field(candidate, "kind") == "widget" &&
                        string_field(candidate, "name") == "Slot") {
                        fill_call = candidate;
                    }
                }
                if (!fill_call) continue;
                projected_statement_indices.push_back(statement_index);

                const JsonValue name_expression = required(fill_call, "arguments").find("name");
                if (!name_expression) continue;
                const runtime::ExpressionValue name_value = evaluate(
                    name_expression,
                    projection_scope.expressions
                );
                const runtime::Value* scalar = name_value.value();
                if (scalar == nullptr || scalar->string() == nullptr || scalar->string()->empty()) {
                    continue;
                }
                const JsonValue fill_children = required(fill_call, "children");
                projected_content.insert_or_assign(
                    *scalar->string(),
                    fill_children.is_null()
                        ? std::vector<std::shared_ptr<const DescriptionNode>>{}
                        : build_block(fill_children, projection_scope)
                );
            }
        }
        std::vector<std::shared_ptr<const DescriptionNode>> raw_content;
        if (call_statement_count > projected_statement_indices.size()) {
            raw_content = build_block(
                call_children,
                projection_scope,
                projected_statement_indices
            );
        }

        Scope component_scope;
        component_scope.declaration_scope = "component " + type;
        component_scope.instance_path = component_instance_path(
            scope.instance_path,
            type,
            key,
            source_path
        );
        component_scope.runtime_state_scope = component_scope.instance_path;
        component_scope.expressions.contextual_host_roots =
            scope.expressions.contextual_host_roots;
        component_scope.expressions.component_path = component_scope.instance_path;
        component_scope.expressions.state_bindings = scope.expressions.state_bindings;
        component_scope.widget_defaults = projection_scope.widget_defaults;

        const JsonArray parameters = array_field(component, "parameters");
        for (const JsonValue parameter : parameters) {
            const JsonValue schema = required(parameter, "schema");
            const std::string name(string_field(schema, "name"));
            const auto supplied = properties.find(name);
            if (supplied != properties.end()) {
                const std::optional<bool> state_binding =
                    required(schema, "stateBinding").boolean();
                if (!state_binding.has_value()) {
                    throw std::logic_error(
                        "validated component parameter binding flag changed type"
                    );
                }
                bind_expression(
                    component_scope.expressions,
                    name,
                    supplied->second,
                    *state_binding
                );
            } else {
                const JsonValue default_value = required(parameter, "default");
                const runtime::ExpressionValue evaluated = default_value.is_null()
                    ? runtime::ExpressionValue(runtime::Value{})
                    : evaluate(default_value, component_scope.expressions);
                bind_expression(component_scope.expressions, name, evaluated);
            }
        }
        DescriptionNode::Properties cache_properties = properties;
        for (const auto& [name, value] : component_scope.expressions.values) {
            cache_properties.insert_or_assign(
                "$parameter:" + name,
                runtime::ExpressionValue(value)
            );
        }
        for (const auto& [name, value] : component_scope.expressions.executable_values) {
            cache_properties.insert_or_assign("$parameter:" + name, value);
        }
        const std::optional<ComponentInputs> inputs = component_inputs(
            cache_properties,
            component_scope.widget_defaults
        );
        const std::string cache_key = type + "\n" + component_scope.instance_path;
        std::shared_ptr<const DescriptionNode> component_root;
        if (inputs.has_value()) {
            visited_component_cache_keys_.insert(cache_key);
            for (ComponentEffects* const component_effect : component_effect_stack_) {
                component_effect->descendant_cache_keys.insert(cache_key);
            }
            if (!component_effect_stack_.empty()) {
                component_effect_stack_.back()->direct_descendant_cache_keys.insert(cache_key);
            }
            auto cached = component_cache_.find(cache_key);
            if (cached != component_cache_.end() &&
                cached->second.component == type &&
                cached->second.source_path == source_path &&
                cached->second.inputs == *inputs &&
                cached->second.contextual_host_roots == contextual_host_roots_) {
                static_cast<void>(refresh_component_cache_entry(cache_key));
                cached = component_cache_.find(cache_key);
            }
            if (cached != component_cache_.end() && component_cache_entry_current(
                    cached->second, type, source_path, *inputs
                )) {
                cached->second.host_invalidation_count =
                    application_.host().invalidation_count();
                cached->second.last_used_epoch = component_cache_epoch_;
                replay_component_effects(cached->second.effects);
                component_root = cached->second.root;
            }
        }
        if (component_root == nullptr) {
            ComponentEffects effects;
            const std::size_t diagnostics_before = diagnostics_.size();
            Scope rebuild_scope = component_scope;
            component_root = build_component_body(
                type,
                std::move(component_scope),
                effects
            );
            if (inputs.has_value() && diagnostics_.size() == diagnostics_before) {
                component_cache_.insert_or_assign(cache_key, ComponentCacheEntry{
                    type,
                    source_path,
                    *inputs,
                    application_.host().invalidation_count(),
                    component_cache_epoch_,
                    contextual_host_roots_,
                    std::move(effects),
                    component_root,
                    std::move(rebuild_scope),
                });
            } else {
                if (inputs.has_value()) {
                    for (ComponentEffects* const component_effect :
                         component_effect_stack_) {
                        component_effect->direct_descendant_cache_keys.erase(cache_key);
                        component_effect->descendant_cache_keys.erase(cache_key);
                    }
                }
                absorb_uncached_component_effects(effects);
                component_cache_.erase(cache_key);
            }
        }
        if (!call_children.is_null()) {
            std::set<std::string, std::less<>> declared_slots;
            collect_slot_names(component_root, declared_slots);
            if (!raw_content.empty()) {
                const std::string shorthand = declared_slots.size() == 1U
                                                  ? *declared_slots.begin()
                                                  : declared_slots.contains("content")
                                                      ? std::string("content")
                                                      : std::string{};
                if (!shorthand.empty()) {
                    projected_content.insert_or_assign(
                        shorthand,
                        std::move(raw_content)
                    );
                }
            }
            for (auto content = projected_content.begin(); content != projected_content.end();) {
                if (!declared_slots.contains(content->first)) {
                    content = projected_content.erase(content);
                } else {
                    ++content;
                }
            }
            if (!projected_content.empty()) {
                component_root = project_slots(component_root, projected_content);
            }
        }
        if (behaviors.empty()) return component_root;

        auto expanded = std::make_shared<DescriptionNode>(*component_root);
        for (DescriptionBehavior& behavior : behaviors) {
            const auto duplicate = std::ranges::find(
                expanded->behaviors,
                behavior.id,
                &DescriptionBehavior::id
            );
            if (duplicate != expanded->behaviors.end()) {
                throw std::logic_error("component call attaches a duplicate root behavior");
            }
            expanded->behaviors.push_back(std::move(behavior));
        }
        return expanded;
    }

    std::vector<std::shared_ptr<const DescriptionNode>> child_nodes;
    if (!children_value.is_null() && repeater_children.source == nullptr) {
        child_nodes = build_block(children_value, scope);
    }
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
        scope.runtime_state_scope,
        application_.bundle()->action_registry(),
        retained,
        [this, scope](
            const std::string_view component,
            std::string template_key,
            WidgetTemplateArguments arguments
        ) {
            return build_component_template(
                component,
                std::move(template_key),
                std::move(arguments),
                scope
            );
        },
        component_effect_stack_.empty()
            ? WidgetRetainedDependencyObserver{}
            : WidgetRetainedDependencyObserver{
                  [this, retained_query](
                      const std::string_view name,
                      const runtime::Value* const value
                  ) {
                      observe_retained_value(retained_query, name, value);
                  }
              }
    );
    std::shared_ptr<const DescriptionChildren> generated_children =
        std::move(expansion.generated_children);
    WidgetGeneratedVirtualization generated_virtualization;
    if (expansion.generated_widget_children != nullptr) {
        if (generated_children != nullptr) {
            throw std::logic_error(
                "widget description installed two generated child providers"
            );
        }
        const std::shared_ptr<const WidgetGeneratedChildren> generated =
            std::move(expansion.generated_widget_children);
        generated_virtualization = generated->virtualization;
        capture_retained_snapshot();
        auto evaluation = std::make_shared<WidgetRowEvaluationState>(
            application_,
            widgets_,
            scope,
            generated->factory,
            retained_snapshot_
        );
        generated_children = std::make_shared<const GeneratedDescriptionChildren>(
            generated->count,
            [evaluation = std::move(evaluation)](const std::size_t index) {
                return evaluation->materialize(index);
            }
        );
    }
    type = std::move(expansion.type);
    key = std::move(expansion.key);
    properties = std::move(expansion.properties);
    child_nodes = std::move(expansion.children);
    behaviors = std::move(expansion.behaviors);
    if (repeater_children.source != nullptr) {
        set_layout_field(properties, "virtualMeasureItemExtents", runtime::Value(true));
    }
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
            const auto copy_layout_field = [&source_layout, &item_layout_fields](
                                               const std::string_view name
                                           ) {
                const runtime::Value* value = source_layout != nullptr
                                                  ? source_layout->field(name)
                                                  : nullptr;
                if (value != nullptr) item_layout_fields.emplace_back(std::string(name), *value);
            };
            copy_layout_field("kind");
            copy_layout_field("gap");
            copy_layout_field("alignItems");
            copy_layout_field("justifyContent");
            copy_layout_field("alignContent");
            copy_layout_field("wrap");
            item_layout_fields.emplace_back(
                "width",
                runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                    {"weight", runtime::Value(1.0)},
                })
            );
            item_layout_fields.emplace_back("height", runtime::Value("content"));
            DescriptionNode::Properties item_properties;
            item_properties.emplace(
                "$layout",
                runtime::ExpressionValue(runtime::Value(std::move(item_layout_fields)))
            );
            item_properties.emplace(
                "transition",
                runtime::ExpressionValue(*content_transition_property->second.value())
            );
            const auto content_transition_mode = properties.find("contentTransitionMode");
            const runtime::Value* transition_mode = content_transition_mode != properties.end()
                                                        ? content_transition_mode->second.value()
                                                        : nullptr;
            item_properties.emplace(
                "$transitionSequence",
                runtime::ExpressionValue(runtime::Value(
                    transition_mode != nullptr && transition_mode->string() != nullptr
                        ? *transition_mode->string()
                        : "OUT_IN"
                ))
            );
            auto item = DescriptionNode::create(
                "AnimatedContentItem",
                *content_key,
                source_path,
                scope.runtime_state_scope,
                std::move(item_properties),
                std::make_shared<const EagerDescriptionChildren>(std::move(child_nodes))
            );
            DescriptionNode::Properties coordinator_properties;
            coordinator_properties.emplace(
                "$layout",
                runtime::ExpressionValue(runtime::Value(
                    std::vector<std::pair<std::string, runtime::Value>>{
                        {"clip", runtime::Value(true)},
                        {"height", runtime::Value("content")},
                        {"kind", runtime::Value("STACK")},
                        {"width", runtime::Value(
                            std::vector<std::pair<std::string, runtime::Value>>{
                                {"weight", runtime::Value(1.0)},
                            }
                        )},
                    }
                ))
            );
            coordinator_properties.emplace(
                "animateContentSize",
                runtime::ExpressionValue(runtime::Value(
                    std::vector<std::pair<std::string, runtime::Value>>{
                        {"clip", runtime::Value(true)},
                        {"height", runtime::Value(true)},
                        {"width", runtime::Value(false)},
                    }
                ))
            );
            auto coordinator = DescriptionNode::create(
                "AnimatedContent",
                "strata.content." + key.value_or(source_path),
                source_path,
                scope.runtime_state_scope,
                std::move(coordinator_properties),
                std::make_shared<const EagerDescriptionChildren>(
                    std::vector<std::shared_ptr<const DescriptionNode>>{std::move(item)}
                )
            );
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
        type,
        key,
        source_path,
        scope.runtime_state_scope,
        std::move(properties),
        generated_children != nullptr
            ? std::move(generated_children)
            : std::shared_ptr<const DescriptionChildren>(
                  std::make_shared<const EagerDescriptionChildren>(std::move(child_nodes))
              ),
        std::move(behaviors)
    );
    if (generated_virtualization.sequence != nullptr ||
        generated_virtualization.item_members != nullptr ||
        generated_virtualization.item_extents != nullptr ||
        repeater_children.sequence != nullptr) {
        auto virtualized = std::make_shared<DescriptionNode>(*result);
        virtualized->virtual_sequence = generated_virtualization.sequence != nullptr
            ? std::move(generated_virtualization.sequence)
            : std::move(repeater_children.sequence);
        virtualized->virtual_sequence_generation = std::move(repeater_children.generation);
        virtualized->virtual_item_members = std::move(
            generated_virtualization.item_members
        );
        virtualized->virtual_item_extents = std::move(
            generated_virtualization.item_extents
        );
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

std::shared_ptr<const DescriptionNode> DescriptionBuilder::build_component_template(
    const std::string_view component_name,
    std::string key,
    WidgetTemplateArguments arguments,
    const Scope& caller
) {
    const JsonValue component = application_.active_unit()->component(component_name);
    if (!component) {
        throw std::logic_error(
            "component template '" + std::string(component_name) + "' lost its declaration"
        );
    }
    Scope component_scope;
    component_scope.declaration_scope = "component " + std::string(component_name);
    component_scope.instance_path = component_instance_path(
        caller.instance_path,
        component_name,
        key,
        "$template:" + key
    );
    component_scope.runtime_state_scope = component_scope.instance_path;
    component_scope.expressions.contextual_host_roots =
        caller.expressions.contextual_host_roots;
    component_scope.expressions.component_path = component_scope.instance_path;
    component_scope.expressions.state_bindings = caller.expressions.state_bindings;
    component_scope.widget_defaults = caller.widget_defaults;
    for (const JsonValue entry : array_field(component, "widgetDefaults")) {
        Scope::WidgetDefault defaults;
        const JsonValue style = required(entry, "style");
        const JsonValue variant = required(entry, "variant");
        if (!style.is_null()) defaults.style = evaluate(style, caller.expressions);
        if (!variant.is_null()) defaults.variant = evaluate(variant, caller.expressions);
        component_scope.widget_defaults.insert_or_assign(
            std::string(string_field(entry, "name")),
            std::move(defaults)
        );
    }
    for (const JsonValue parameter : array_field(component, "parameters")) {
        const JsonValue schema = required(parameter, "schema");
        const std::string name(string_field(schema, "name"));
        const auto supplied = arguments.find(name);
        if (supplied != arguments.end()) {
            const std::optional<bool> state_binding =
                required(schema, "stateBinding").boolean();
            if (!state_binding.has_value()) {
                throw std::logic_error(
                    "validated component template parameter binding flag changed type"
                );
            }
            bind_expression(
                component_scope.expressions,
                name,
                supplied->second,
                *state_binding
            );
            continue;
        }
        const JsonValue default_value = required(parameter, "default");
        const runtime::ExpressionValue evaluated = default_value.is_null()
            ? runtime::ExpressionValue(runtime::Value{})
            : evaluate(default_value, component_scope.expressions);
        bind_expression(component_scope.expressions, name, evaluated);
    }
    std::vector<std::shared_ptr<const DescriptionNode>> roots = build_block(
        required(component, "body"),
        std::move(component_scope)
    );
    if (roots.size() != 1U) {
        throw std::logic_error("validated component template must describe exactly one root node");
    }
    return std::move(roots.front());
}

std::vector<DescriptionBehavior> DescriptionBuilder::build_behaviors(
    const JsonValue expression,
    const runtime::ExpressionScope& scope
) {
    std::vector<DescriptionBehavior> result;
    if (string_field(expression, "kind") != "list") return result;
    for (const JsonValue element : array_field(expression, "elements")) {
        if (string_field(element, "kind") != "map") continue;
        const JsonValue entries = required(element, "entries");
        const JsonValue id_expression = entries.find("id");
        if (!id_expression) continue;
        const runtime::ExpressionValue id_value = evaluate(id_expression, scope);
        const runtime::Value* id_scalar = id_value.value();
        if (id_scalar == nullptr || id_scalar->string() == nullptr || id_scalar->string()->empty()) continue;

        DescriptionBehavior behavior;
        behavior.id = *id_scalar->string();
        if (const JsonValue enabled_expression = entries.find("enabled"); enabled_expression) {
            const runtime::ExpressionValue enabled_value = evaluate(enabled_expression, scope);
            if (enabled_value.value() != nullptr && enabled_value.value()->boolean() != nullptr) {
                behavior.enabled = *enabled_value.value()->boolean();
            }
        }
        if (const JsonValue options_expression = entries.find("options"); options_expression) {
            behavior.options = require_value(evaluate(options_expression, scope), options_expression);
        } else {
            behavior.options = runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{});
        }
        if (const JsonValue action_expression = entries.find("action"); action_expression) {
            const runtime::ExpressionValue action_value = evaluate(action_expression, scope);
            if (action_value.action() != nullptr && *action_value.action() != nullptr) {
                behavior.action = *action_value.action();
            }
        }
        result.push_back(std::move(behavior));
    }
    return result;
}

void DescriptionBuilder::bind_scope_state(const Scope& scope) {
    for (const auto& [name, binding] : scope.expressions.state_bindings) {
        bind_state_scope(
            scope.runtime_state_scope,
            name,
            binding.declaration_scope,
            binding.address.scope
        );
    }
}

void DescriptionBuilder::bind_state_scope(
    const std::string_view runtime_scope,
    const std::string_view state_name,
    const std::string_view declaration_scope,
    const std::string_view address_scope
) {
    const runtime::StateAddress binding_address{
        std::string(runtime_scope),
        std::string(state_name),
    };
    const StateBindingEffect effect{
        std::string(declaration_scope),
        std::string(address_scope),
    };
    for (ComponentEffects* const component : component_effect_stack_) {
        component->state_bindings.insert_or_assign(binding_address, effect);
    }
    application_.bind_state_scope(
        binding_address.scope,
        binding_address.name,
        effect.declaration_scope,
        effect.address_scope
    );
}

void DescriptionBuilder::own_state_scope(const std::string_view scope) {
    current_layer_state_scopes_.insert(std::string(scope));
    application_.state().mark_owned_scope(std::string(scope));
    for (ComponentEffects* const component : component_effect_stack_) {
        component->owned_state_scopes.insert(std::string(scope));
    }
}

void DescriptionBuilder::observe_state_value(
    const runtime::StateAddress& address,
    const runtime::Value& value
) {
    if (!component_effect_stack_.empty()) {
        component_effect_stack_.back()->state_values.insert_or_assign(address, value);
    }
}

void DescriptionBuilder::observe_host_dependency(
    const runtime::ExpressionHostDependency& dependency
) {
    const std::string path = runtime::canonical_host_dependency_path(dependency.path);
    if (!component_effect_stack_.empty()) {
        component_effect_stack_.back()->host_values.insert_or_assign(path, dependency);
    }
}

void DescriptionBuilder::observe_retained_value(
    const RetainedQuery& query,
    const std::string_view name,
    const runtime::Value* const value
) {
    const std::optional<runtime::Value> observed = value != nullptr
                                                       ? std::optional(*value)
                                                       : std::nullopt;
    if (component_effect_stack_.empty()) return;
    ComponentEffects& component = *component_effect_stack_.back();
    auto effect = std::ranges::find(
        component.retained_values,
        query,
        &RetainedValueEffects::query
    );
    if (effect == component.retained_values.end()) {
        component.retained_values.push_back(RetainedValueEffects{query, {}});
        effect = std::prev(component.retained_values.end());
    }
    effect->values.insert_or_assign(std::string(name), observed);
}

void DescriptionBuilder::observe_retained_sequence(
    const RetainedQuery& query,
    const RetainedDescriptionSnapshot::Node* const retained
) {
    if (component_effect_stack_.empty()) return;
    const std::shared_ptr<const runtime::IndexableSequence> sequence =
        retained != nullptr ? retained->virtual_sequence.lock() : nullptr;
    const std::optional<DescriptionSequenceGeneration> generation =
        retained != nullptr ? retained->virtual_sequence_generation : std::nullopt;
    ComponentEffects& component = *component_effect_stack_.back();
    auto effect = std::ranges::find(
        component.retained_sequences,
        query,
        &RetainedSequenceEffect::query
    );
    if (effect == component.retained_sequences.end()) {
        component.retained_sequences.push_back(RetainedSequenceEffect{
            query,
            sequence,
            generation,
        });
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
}

std::shared_ptr<const DescriptionNode> DescriptionBuilder::build_component_body(
    const std::string_view component,
    Scope scope,
    ComponentEffects& effects
) {
    const JsonValue declaration = application_.active_unit()->component(component);
    if (!declaration) {
        throw std::logic_error(
            "cached component '" + std::string(component) + "' lost its declaration"
        );
    }
    component_effect_stack_.push_back(&effects);
    ComponentExpressionDependencies dependencies;
    dependencies.observe_host = [this](
        const runtime::ExpressionHostDependency& dependency
    ) {
        observe_host_dependency(dependency);
    };
    ExpressionDependencyObserverRestore dependency_observer(
        *expressions_,
        &dependencies
    );
    dependencies.upstream = dependency_observer.previous();
    std::vector<std::shared_ptr<const DescriptionNode>> roots;
    try {
        roots = build_block(required(declaration, "body"), std::move(scope));
    } catch (...) {
        component_effect_stack_.pop_back();
        throw;
    }
    component_effect_stack_.pop_back();
    if (roots.size() != 1U) {
        throw std::logic_error(
            "validated component body must describe exactly one root node"
        );
    }
    return std::move(roots.front());
}

std::shared_ptr<const DescriptionNode> DescriptionBuilder::replace_component_subtrees(
    const std::shared_ptr<const DescriptionNode>& root,
    const std::map<
        const DescriptionNode*,
        std::shared_ptr<const DescriptionNode>
    >& replacements
) {
    if (root == nullptr || replacements.empty()) return root;
    std::set<const DescriptionNode*> applied;
    const auto replace = [&replacements, &applied](
                             const auto& self,
                             const std::shared_ptr<const DescriptionNode>& current
                         ) -> std::shared_ptr<const DescriptionNode> {
        if (const auto replacement = replacements.find(current.get());
            replacement != replacements.end()) {
            applied.insert(replacement->first);
            return replacement->second;
        }
        if (current->materialization.has_value()) return current;
        bool changed = false;
        std::vector<std::shared_ptr<const DescriptionNode>> children;
        children.reserve(current->children->size());
        for (std::size_t index = 0U; index < current->children->size(); ++index) {
            const std::shared_ptr<const DescriptionNode> child =
                current->children->at(index);
            std::shared_ptr<const DescriptionNode> next = self(self, child);
            changed = changed || next != child;
            children.push_back(std::move(next));
        }
        if (!changed) return current;
        auto result = std::make_shared<DescriptionNode>(*current);
        result->children = std::make_shared<const EagerDescriptionChildren>(
            std::move(children)
        );
        return result;
    };
    std::shared_ptr<const DescriptionNode> result = replace(replace, root);
    // A loop or slot projection may have cloned a component root to annotate it. In that case a
    // partial pointer rewrite would leave stale descendants, so the caller must use its rebuild.
    if (applied.size() != replacements.size()) return nullptr;
    return result;
}

DescriptionBuilder::ComponentRefreshResult
DescriptionBuilder::refresh_component_cache_entry(const std::string& cache_key) {
    auto found = component_cache_.find(cache_key);
    if (found == component_cache_.end()) return ComponentRefreshResult::invalid;
    if (!refreshing_component_cache_keys_.insert(cache_key).second) {
        return ComponentRefreshResult::unchanged;
    }
    struct RefreshGuard final {
        std::set<std::string, std::less<>>& keys;
        const std::string& key;
        ~RefreshGuard() { keys.erase(key); }
    } guard{refreshing_component_cache_keys_, cache_key};

    ComponentCacheEntry& entry = found->second;
    std::map<const DescriptionNode*, std::shared_ptr<const DescriptionNode>> replacements;
    for (const std::string& child_key : entry.effects.direct_descendant_cache_keys) {
        const auto child_before = component_cache_.find(child_key);
        if (child_before == component_cache_.end()) {
            return ComponentRefreshResult::invalid;
        }
        const std::shared_ptr<const DescriptionNode> previous = child_before->second.root;
        const ComponentRefreshResult refreshed =
            refresh_component_cache_entry(child_key);
        const auto child_after = component_cache_.find(child_key);
        if (refreshed == ComponentRefreshResult::invalid ||
            child_after == component_cache_.end()) {
            return ComponentRefreshResult::invalid;
        }
        if (child_after->second.root != previous) {
            replacements.insert_or_assign(
                previous.get(),
                child_after->second.root
            );
        }
    }

    const bool direct_current = component_cache_entry_current(
        entry,
        entry.component,
        entry.source_path,
        entry.inputs
    );
    if (replacements.empty() && direct_current) {
        return ComponentRefreshResult::unchanged;
    }
    const std::shared_ptr<const DescriptionNode> patched =
        direct_current && !replacements.empty()
            ? replace_component_subtrees(entry.root, replacements)
            : nullptr;

    const std::size_t diagnostics_before = diagnostics_.size();
    ComponentEffects effects;
    std::shared_ptr<const DescriptionNode> rebuilt = build_component_body(
        entry.component,
        entry.rebuild_scope,
        effects
    );
    if (diagnostics_.size() != diagnostics_before) {
        component_cache_.erase(cache_key);
        return ComponentRefreshResult::invalid;
    }
    entry.host_invalidation_count = application_.host().invalidation_count();
    entry.last_used_epoch = component_cache_epoch_;
    entry.effects = std::move(effects);
    entry.root = patched != nullptr ? patched : std::move(rebuilt);
    return ComponentRefreshResult::changed;
}

void DescriptionBuilder::replay_component_effects(const ComponentEffects& effects) {
    for (const std::string& cache_key : effects.descendant_cache_keys) {
        visited_component_cache_keys_.insert(cache_key);
        for (ComponentEffects* const component : component_effect_stack_) {
            component->descendant_cache_keys.insert(cache_key);
        }
    }
    for (const std::string& scope : effects.owned_state_scopes) own_state_scope(scope);
    for (const auto& [binding_address, binding] : effects.state_bindings) {
        bind_state_scope(
            binding_address.scope,
            binding_address.name,
            binding.declaration_scope,
            binding.address_scope
        );
    }
}

void DescriptionBuilder::absorb_uncached_component_effects(
    const ComponentEffects& effects
) {
    if (component_effect_stack_.empty()) return;
    ComponentEffects& parent = *component_effect_stack_.back();
    parent.host_values.insert(effects.host_values.begin(), effects.host_values.end());
    parent.state_values.insert(effects.state_values.begin(), effects.state_values.end());
    parent.state_bindings.insert(
        effects.state_bindings.begin(),
        effects.state_bindings.end()
    );
    parent.owned_state_scopes.insert(
        effects.owned_state_scopes.begin(),
        effects.owned_state_scopes.end()
    );
    parent.direct_descendant_cache_keys.insert(
        effects.direct_descendant_cache_keys.begin(),
        effects.direct_descendant_cache_keys.end()
    );
    parent.descendant_cache_keys.insert(
        effects.descendant_cache_keys.begin(),
        effects.descendant_cache_keys.end()
    );
    for (const RetainedValueEffects& source : effects.retained_values) {
        auto destination = std::ranges::find(
            parent.retained_values,
            source.query,
            &RetainedValueEffects::query
        );
        if (destination == parent.retained_values.end()) {
            parent.retained_values.push_back(source);
            continue;
        }
        destination->values.insert(source.values.begin(), source.values.end());
    }
    for (const RetainedSequenceEffect& source : effects.retained_sequences) {
        auto destination = std::ranges::find(
            parent.retained_sequences,
            source.query,
            &RetainedSequenceEffect::query
        );
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

std::optional<DescriptionBuilder::ComponentInputs> DescriptionBuilder::component_inputs(
    const DescriptionNode::Properties& properties,
    const std::map<std::string, Scope::WidgetDefault, std::less<>>& widget_defaults
) const {
    ComponentInputs result;
    result.reserve(properties.size() + widget_defaults.size() * 2U);
    const auto append = [&result](std::string name, const runtime::ExpressionValue& value) {
        runtime::ExpressionDependencyValue dependency =
            runtime::capture_expression_dependency(value);
        if (!dependency.cacheable()) return false;
        result.emplace_back(std::move(name), std::move(dependency));
        return true;
    };
    for (const auto& [name, value] : properties) {
        if (!append("argument:" + name, value)) return std::nullopt;
    }
    for (const auto& [widget, defaults] : widget_defaults) {
        if (defaults.style.has_value() &&
            !append("default:" + widget + ":style", *defaults.style)) {
            return std::nullopt;
        }
        if (defaults.variant.has_value() &&
            !append("default:" + widget + ":variant", *defaults.variant)) {
            return std::nullopt;
        }
    }
    return result;
}

bool DescriptionBuilder::component_cache_entry_current(
    const ComponentCacheEntry& entry,
    const std::string_view component,
    const std::string_view source_path,
    const ComponentInputs& inputs
) const {
    if (entry.component != component || entry.source_path != source_path ||
        entry.inputs != inputs || entry.contextual_host_roots != contextual_host_roots_) {
        return false;
    }
    return component_effects_current(
        entry.effects,
        entry.host_invalidation_count,
        entry.contextual_host_roots
    );
}

bool DescriptionBuilder::component_effects_current(
    const ComponentEffects& effects,
    const std::uint64_t host_invalidation_count,
    const std::map<std::string, runtime::Value, std::less<>>& contextual_host_roots
) const {
    if (contextual_host_roots != contextual_host_roots_) return false;
    if (effects.captures_retained_snapshot &&
        effects.retained_snapshot != retained_snapshot_) {
        return false;
    }
    if (host_invalidation_count != application_.host().invalidation_count()) {
        for (const auto& [path, dependency] : effects.host_values) {
            static_cast<void>(path);
            if (dependency.contextual) continue;
            const std::optional<std::pair<std::string, std::uint64_t>> origin =
                application_.host().origin(dependency.path);
            if (origin.has_value() != dependency.snapshot_id.has_value() ||
                (origin.has_value() &&
                 (origin->first != *dependency.snapshot_id ||
                  origin->second != dependency.snapshot_generation))) {
                return false;
            }
        }
    }
    for (const auto& [address, value] : effects.state_values) {
        const runtime::Value* const current = application_.state().find(address);
        if (current == nullptr || *current != value) return false;
    }
    for (const RetainedValueEffects& effect : effects.retained_values) {
        const RetainedDescriptionSnapshot::Node* const retained = retained_widget(effect.query);
        for (const auto& [name, expected] : effect.values) {
            const runtime::Value* const current = retained != nullptr
                                                      ? retained->retained_value(name)
                                                      : nullptr;
            if (current == nullptr) {
                if (expected.has_value()) return false;
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
        if (sequence != effect.sequence.lock() || generation != effect.generation) return false;
    }
    return true;
}

runtime::ExpressionValue DescriptionBuilder::evaluate(
    const JsonValue expression,
    const runtime::ExpressionScope& scope
) {
    ++evaluated_expressions_;
    runtime::ExpressionValue value = expressions_->evaluate_in(expression, scope);
    append_diagnostics(*expressions_);
    return value;
}

runtime::Value DescriptionBuilder::require_value(
    const runtime::ExpressionValue& value,
    const JsonValue expression
) {
    if (value.value() != nullptr) return *value.value();
    if (value.collection() != nullptr) return (*value.collection())->items;
    const std::optional<std::string_view> expression_path = expression.find("path").string();
    diagnostics_.push_back(runtime::RuntimeDiagnostic{
        "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
        "Description expression did not produce a scalar value.",
        expression_path.has_value() ? std::string(*expression_path) : std::string{},
        std::string("scalar value"),
        runtime::DiagnosticSeverity::error,
        runtime::portable_expression_range(expression),
    });
    return runtime::Value{};
}

void DescriptionBuilder::append_diagnostics(runtime::ExpressionRuntime& expressions) {
    diagnostics_.insert(
        diagnostics_.end(),
        expressions.diagnostics().begin(),
        expressions.diagnostics().end()
    );
    expressions.clear_diagnostics();
}

runtime::Value DescriptionBuilder::resolve_style(
    const runtime::Value& value,
    const runtime::ExpressionScope& scope,
    std::set<std::string, std::less<>>& resolving
) {
    if (value.string() != nullptr) return resolve_named_style(*value.string(), scope, resolving);
    if (value.object() == nullptr) return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{});
    std::map<std::string, runtime::Value, std::less<>> merged;
    if (const runtime::Value* bases = value.field("$bases"); bases != nullptr && bases->list() != nullptr) {
        for (const runtime::Value& base : bases->list()->values) {
            merge_object(merged, resolve_style(base, scope, resolving));
        }
    }
    merge_object(merged, value);
    return map_value(std::move(merged));
}

runtime::Value DescriptionBuilder::resolve_named_style(
    const std::string_view name,
    const runtime::ExpressionScope& scope,
    std::set<std::string, std::less<>>& resolving
) {
    if (const auto cached = resolved_styles_.find(name); cached != resolved_styles_.end()) return cached->second;
    const auto [resolving_entry, inserted] = resolving.emplace(name);
    if (!inserted) {
        throw std::logic_error("validated portable IR contains a cyclic style inheritance chain");
    }
    const JsonValue declaration = application_.active_unit()->style(name);
    if (!declaration) {
        resolving.erase(resolving_entry);
        return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{});
    }
    std::map<std::string, runtime::Value, std::less<>> merged;
    for (const JsonValue base : array_field(declaration, "bases")) {
        if (const std::optional<std::string_view> base_name = base.string(); base_name.has_value()) {
            merge_object(merged, resolve_named_style(*base_name, scope, resolving));
        }
    }
    for (const auto [property, expression] : object_field(declaration, "properties")) {
        merged.insert_or_assign(
            std::string(property),
            require_value(evaluate(expression, scope), expression)
        );
    }
    resolving.erase(resolving_entry);
    runtime::Value result = map_value(std::move(merged));
    resolved_styles_.emplace(std::string(name), result);
    return result;
}

void DescriptionBuilder::normalize_layout(
    DescriptionNode::Properties& properties,
    const runtime::ExpressionScope& scope
) {
    std::map<std::string, runtime::Value, std::less<>> merged;
    bool present = false;
    std::set<std::string, std::less<>> resolving;
    if (const auto style = properties.find("style"); style != properties.end() && style->second.value() != nullptr) {
        merge_object(merged, resolve_style(*style->second.value(), scope, resolving));
        present = true;
    }
    if (const auto layout = properties.find("layout"); layout != properties.end() && layout->second.value() != nullptr) {
        merge_object(merged, *layout->second.value());
        present = true;
    }
    if (present) properties.insert_or_assign("$layout", runtime::ExpressionValue(map_value(std::move(merged))));
}

} // namespace strata::ui
