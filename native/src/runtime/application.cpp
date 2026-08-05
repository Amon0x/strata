#include "runtime/application.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"
#include "runtime/expression.hpp"

namespace strata::runtime {
namespace {

[[nodiscard]] ActivationDiagnostic activation_diagnostic(const compiler::Diagnostic& value) {
    const auto severity = [&value] {
        switch (value.severity) {
        case compiler::DiagnosticSeverity::info: return DiagnosticSeverity::info;
        case compiler::DiagnosticSeverity::warning: return DiagnosticSeverity::warning;
        case compiler::DiagnosticSeverity::error: return DiagnosticSeverity::error;
        }
        return DiagnosticSeverity::error;
    }();
    std::optional<DiagnosticRange> range;
    if (value.range.has_value()) {
        range = DiagnosticRange{
            value.range->source_id,
            DiagnosticPosition{
                value.range->start.line,
                value.range->start.column,
                value.range->start.offset,
            },
            DiagnosticPosition{
                value.range->end.line,
                value.range->end.column,
                value.range->end.offset,
            },
        };
    }
    return ActivationDiagnostic{
        value.code,
        value.message,
        value.range.has_value() ? std::optional(value.range->source_id) : std::nullopt,
        value.component_path,
        value.expected,
        severity,
        std::move(range),
    };
}

[[nodiscard]] ValueSchemaPtr state_schema(const std::string_view type_id) {
    if (type_id == "dsl.null") return ValueSchema::scalar(ValueSchemaKind::null_value);
    if (type_id == "dsl.boolean") return ValueSchema::scalar(ValueSchemaKind::boolean);
    if (type_id == "dsl.number") return ValueSchema::scalar(ValueSchemaKind::number);
    if (type_id == "dsl.duration") return ValueSchema::scalar(ValueSchemaKind::duration);
    if (type_id == "dsl.string") return ValueSchema::scalar(ValueSchemaKind::string);
    if (type_id == "dsl.color") return ValueSchema::scalar(ValueSchemaKind::color);
    if (type_id == "dsl.image") return ValueSchema::scalar(ValueSchemaKind::image);
    if (type_id == "dsl.key") return ValueSchema::scalar(ValueSchemaKind::key);
    if (type_id == "dsl.theme-token") return ValueSchema::scalar(ValueSchemaKind::theme_token);
    if (type_id == "dsl.list") return ValueSchema::list(ValueSchema::any(), true, 100'000U);
    if (type_id == "dsl.map") return ValueSchema::object({}, true, ValueSchema::any());
    return ValueSchema::any();
}

[[nodiscard]] ActionDispatchOutcome framework_failure(
    const Action& action,
    std::string message
) {
    return ActionDispatchOutcome{
        ActionDispatchStatus::failed,
        action.id(),
        {},
        std::move(message),
    };
}

} // namespace

bool ActivationResult::activated() const noexcept { return status == ActivationStatus::activated; }

ApplicationBundle::ApplicationBundle(
    compiler::SchemaRegistry schemas,
    RuntimeActionRegistry actions,
    HostSchemaRoots host_schemas,
    std::set<std::string, std::less<>> async_bindings
)
    : schemas_(std::move(schemas)),
      actions_(std::move(actions)),
      host_schemas_(std::move(host_schemas)),
      async_bindings_(std::move(async_bindings)) {}

std::shared_ptr<const ApplicationBundle> ApplicationBundle::create(
    const data::JsonValue* const application_schemas,
    ActionPayloadDecoders action_decoders,
    const std::span<const data::JsonValue> extension_declarations
) {
    compiler::SchemaRegistry schemas = compiler::SchemaRegistry::builtins();
    for (const data::JsonValue& declaration : extension_declarations) {
        schemas.apply_scenario_declarations(declaration);
    }
    if (application_schemas != nullptr) schemas.apply_scenario_declarations(*application_schemas);
    HostSchemaRoots host_schemas;
    std::set<std::string, std::less<>> async_bindings;
    for (const auto& [path, type] : schemas.host_types()) {
        if (type != nullptr) {
            host_schemas.insert_or_assign(path, runtime_schema(*type));
            if (type->kind == compiler::SemanticTypeKind::async_value) {
                async_bindings.insert(path);
            }
        }
    }
    RuntimeActionRegistry actions = RuntimeActionRegistry::from_schema(
        schemas,
        std::move(action_decoders)
    );
    return std::shared_ptr<const ApplicationBundle>(
        new ApplicationBundle(
            std::move(schemas),
            std::move(actions),
            std::move(host_schemas),
            std::move(async_bindings)
        )
    );
}

const compiler::SchemaRegistry& ApplicationBundle::schema_registry() const noexcept {
    return schemas_;
}

const RuntimeActionRegistry& ApplicationBundle::action_registry() const noexcept {
    return actions_;
}

std::shared_ptr<const HostSnapshot> ApplicationBundle::host_snapshot(
    std::string id,
    const std::uint64_t generation,
    const data::JsonValue& roots
) const {
    return HostSnapshot::from_json(
        std::move(id),
        generation,
        roots,
        host_schemas_
    );
}

bool ApplicationBundle::async_binding(const std::string_view name) const noexcept {
    return async_bindings_.contains(name);
}

ValueSchemaPtr ApplicationBundle::async_value_schema(const std::string_view name) const noexcept {
    const auto root = host_schemas_.find(name);
    if (root == host_schemas_.end() || root->second == nullptr) return {};
    const ValueSchemaField* value = root->second->field("value");
    return value != nullptr ? value->schema : ValueSchemaPtr{};
}

const std::set<std::string, std::less<>>& ApplicationBundle::async_bindings() const noexcept {
    return async_bindings_;
}

ApplicationContext::ApplicationContext(
    std::string id,
    std::shared_ptr<const ApplicationBundle> bundle,
    RuntimeServices::PublishedDiagnosticSink diagnostic_sink
)
    : id_(std::move(id)),
      bundle_(std::move(bundle)),
      state_([this] { invalidate(); }),
      host_([this](const std::uint64_t) { invalidate(); }),
      layers_([this] { invalidate(); }),
      actions_(bundle_ != nullptr ? bundle_->action_registry().dispatcher() : ActionDispatcher{}),
      services_(std::move(diagnostic_sink)),
      undo_([this](const std::string_view scope, const std::string_view action_id) {
          services_.report(RuntimeDiagnostic{
              "STRATA.UNDO.STACK_INVALIDATED",
              "Action '" + std::string(action_id) +
                  "' changed state outside the undo model; the '" + std::string(scope) +
                  "' undo stack was cleared.",
              std::string(scope),
              std::nullopt,
              DiagnosticSeverity::warning,
              std::nullopt,
          });
      }),
      async_(
          [this](const std::string_view binding) {
              return bundle_ != nullptr && bundle_->async_binding(binding);
          },
          [this](const std::string_view binding, const Value& value) -> std::optional<Value> {
              const ValueSchemaPtr schema = bundle_ != nullptr
                  ? bundle_->async_value_schema(binding) : ValueSchemaPtr{};
              return schema != nullptr ? schema->normalize(value) : std::nullopt;
          },
          [this](const Value& roots) {
              if (bundle_ == nullptr) return;
              static_cast<void>(host_.adopt(bundle_->host_snapshot(
                  "strata.runtime.async",
                  ++async_generation_,
                  value_to_json(roots)
              )));
          }
      ),
      profiler_(ProfilerScope::runtime, id_) {
    if (id_.empty() || !core::valid_utf8(id_)) {
        throw std::invalid_argument("application context id must be non-empty valid UTF-8");
    }
    if (bundle_ == nullptr) throw std::invalid_argument("application context bundle must not be null");
    async_.initialize(bundle_->async_bindings());
}

ApplicationContext::~ApplicationContext() {
    static_cast<void>(durability_.flush());
}

const std::string& ApplicationContext::id() const noexcept { return id_; }
const std::shared_ptr<const ApplicationBundle>& ApplicationContext::bundle() const noexcept { return bundle_; }
StateStore& ApplicationContext::state() noexcept { return state_; }
HostStore& ApplicationContext::host() noexcept { return host_; }
LayerStack& ApplicationContext::layers() noexcept { return layers_; }
ActionDispatcher& ApplicationContext::actions() noexcept { return actions_; }
RuntimeServices& ApplicationContext::services() noexcept { return services_; }
const RuntimeServices& ApplicationContext::services() const noexcept { return services_; }
Profiler& ApplicationContext::profiler() noexcept { return profiler_; }
const Profiler& ApplicationContext::profiler() const noexcept { return profiler_; }
DurableState& ApplicationContext::durability() noexcept { return durability_; }
const DurableState& ApplicationContext::durability() const noexcept { return durability_; }
UndoManager& ApplicationContext::undo() noexcept { return undo_; }
const UndoManager& ApplicationContext::undo() const noexcept { return undo_; }
AsyncDataService& ApplicationContext::async() noexcept { return async_; }

bool ApplicationContext::undo_state(const std::string_view scope, const bool redo) {
    const bool changed = redo ? undo_.redo(scope, state_) : undo_.undo(scope, state_);
    if (changed) synchronize_durable_state();
    return changed;
}

void ApplicationContext::synchronize_durable_state() {
    if (active_unit_ == nullptr) return;
    std::set<std::string, std::less<>> synchronized;
    for (const auto& [runtime_address, binding] : state_scope_bindings_) {
        const UnitStateDeclaration* declaration = active_unit_->state_declaration(
            binding.declaration_scope,
            runtime_address.name
        );
        if (declaration == nullptr || !declaration->persistence_key.has_value() ||
            !synchronized.insert(*declaration->persistence_key).second) {
            continue;
        }
        const StateAddress address{binding.address_scope, runtime_address.name};
        if (const Value* value = state_.find(address); value != nullptr) {
            durability_.set_application_value(*declaration->persistence_key, *value);
        } else {
            static_cast<void>(durability_.erase_application_value(*declaration->persistence_key));
        }
    }
}

void ApplicationContext::configure_durable_store(DurableStoreAdapter adapter) {
    for (DurableLoadIssue& issue : durability_.configure(id_, std::move(adapter))) {
        services_.report(RuntimeDiagnostic{
            std::move(issue.code),
            std::move(issue.message),
            id_,
            std::nullopt,
            DiagnosticSeverity::warning,
            std::nullopt,
        });
    }
}

void ApplicationContext::configure_async_host(AsyncHostAdapter adapter) {
    async_.set_adapter(std::move(adapter));
}

void ApplicationContext::flush_durable() {
    if (std::optional<DurableLoadIssue> issue = durability_.flush(); issue.has_value()) {
        services_.publish_current_frame(RuntimeDiagnostic{
            std::move(issue->code),
            std::move(issue->message),
            id_,
            std::nullopt,
            DiagnosticSeverity::warning,
            std::nullopt,
        });
    }
}

ActionDispatchOutcome ApplicationContext::dispatch(
    const ActionEvent& event,
    const Action& action,
    const std::string_view state_scope,
    const LexicalStateBinding* const lexical_state_binding,
    const std::string_view undo_scope,
    const std::int64_t timestamp_nanos
) {
    if (action.contract->dispatch_policy != ActionDispatchPolicy::framework) {
        return actions_.dispatch(event, action);
    }
    const std::string& id = action.id();
    const std::string effective_undo_scope = undo_scope.empty()
        ? std::string("application") : std::string(undo_scope);
    if (id == "application.undo" || id == "application.redo") {
        const UndoStackStatus before = undo_.status(effective_undo_scope);
        const bool changed = undo_state(
            effective_undo_scope,
            id == "application.redo"
        );
        const std::optional<std::string>& label = id == "application.undo"
            ? before.undo_label : before.redo_label;
        return ActionDispatchOutcome{
            changed ? ActionDispatchStatus::handled : ActionDispatchStatus::ignored,
            id,
            {"strata.application.undo"},
            changed && label.has_value()
                ? std::optional<std::string>(
                      std::string(id == "application.undo" ? "Undo " : "Redo ") + *label
                  )
                : std::nullopt,
        };
    }
    if (id == "async.query") {
        const Value* binding_value = action.payload.field("binding");
        const std::string* binding = binding_value != nullptr ? binding_value->string() : nullptr;
        if (binding == nullptr || binding->empty()) {
            return framework_failure(action, "Async query requires a declared binding.");
        }
        const Value* payload = action.payload.field("payload");
        const Value* from_event = action.payload.field("fromEvent");
        const Value* debounce = action.payload.field("debounceMillis");
        const double debounce_millis = debounce != nullptr && debounce->number() != nullptr
            ? *debounce->number() : 0.0;
        if (!std::isfinite(debounce_millis) || debounce_millis < 0.0 ||
            debounce_millis > 86'400'000.0) {
            return framework_failure(
                action, "Async query debounceMillis must be between zero and one day."
            );
        }
        const bool use_event = from_event != nullptr && from_event->boolean() != nullptr &&
            *from_event->boolean();
        std::string owner;
        if (event.lifecycle_owner.has_value() && !event.lifecycle_owner->empty()) {
            owner = effective_undo_scope + ":" + *event.lifecycle_owner;
        } else if (event.source_key.has_value() && !event.source_key->empty()) {
            owner = effective_undo_scope + ":node:" + *event.source_key;
        } else {
            owner = effective_undo_scope + ":state:" +
                (state_scope.empty() ? std::string("application") : std::string(state_scope));
        }
        const std::optional<std::uint64_t> request = async_.query(
            *binding,
            use_event ? event.value : payload != nullptr ? *payload : Value{},
            owner,
            timestamp_nanos,
            static_cast<std::int64_t>(debounce_millis * 1'000'000.0)
        );
        return request.has_value()
            ? ActionDispatchOutcome{ActionDispatchStatus::handled, id, {"strata.application.async"}, std::nullopt}
            : framework_failure(action, "Async query binding '" + *binding + "' is not declared as async.");
    }
    if (id == "async.cancel") {
        const Value* request_value = action.payload.field("requestId");
        const bool changed = request_value != nullptr && request_value->number() != nullptr &&
            *request_value->number() >= 0.0 &&
            async_.cancel(static_cast<std::uint64_t>(*request_value->number()));
        return ActionDispatchOutcome{
            changed ? ActionDispatchStatus::handled : ActionDispatchStatus::ignored,
            id,
            {"strata.application.async"},
            std::nullopt,
        };
    }
    if (active_unit_ == nullptr) {
        return framework_failure(action, "Framework action requires an active runtime unit.");
    }

    if (id.starts_with("state.")) {
        const Value* name_value = action.payload.field("name");
        const std::string* name = name_value != nullptr ? name_value->string() : nullptr;
        if (name == nullptr || name->empty() ||
            (lexical_state_binding == nullptr && state_scope.empty())) {
            return framework_failure(action, "State action requires an owning scope and state name.");
        }
        std::optional<StateScopeResolution> resolved;
        if (lexical_state_binding != nullptr) {
            if (lexical_state_binding->address.name != *name) {
                return framework_failure(
                    action,
                    "Captured state action target does not match payload name '" + *name + "'."
                );
            }
            const UnitStateDeclaration* declaration = active_unit_->state_declaration(
                lexical_state_binding->declaration_scope,
                *name
            );
            if (declaration != nullptr) {
                resolved = StateScopeResolution{
                    lexical_state_binding->address,
                    lexical_state_binding->declaration_scope,
                    declaration,
                };
            }
        } else {
            resolved = resolve_state_scope(state_scope, *name);
        }
        if (!resolved.has_value()) {
            return framework_failure(
                action,
                "State slot '" +
                    (lexical_state_binding != nullptr
                         ? lexical_state_binding->address.scope
                         : std::string(state_scope)) +
                    "/" + *name + "' is not bound."
            );
        }
        const std::string_view declaration_scope = resolved->declaration_scope;
        const UnitStateDeclaration* declaration = resolved->declaration;
        if (declaration == nullptr) {
            return framework_failure(
                action,
                "State slot '" + std::string(state_scope) + "/" + *name + "' is not declared."
            );
        }
        const std::optional<Value> initial = state_initial_value(
            *resolved,
            lexical_state_binding != nullptr
                ? std::string_view(lexical_state_binding->address.scope)
                : state_scope
        );
        if (!initial.has_value()) {
            return framework_failure(action, "State initializer could not be evaluated as a scalar value.");
        }

        StateMutationKind mutation_kind;
        if (id == "state.set" || id == "state.setFromEvent") mutation_kind = StateMutationKind::set;
        else if (id == "state.toggle") mutation_kind = StateMutationKind::toggle;
        else if (id == "state.adjust") mutation_kind = StateMutationKind::adjust;
        else if (id == "state.reset") mutation_kind = StateMutationKind::reset;
        else if (id == "state.listAppend") mutation_kind = StateMutationKind::list_append;
        else if (id == "state.listInsert") mutation_kind = StateMutationKind::list_insert;
        else if (id == "state.listRemoveValue") mutation_kind = StateMutationKind::list_remove_value;
        else if (id == "state.listRemoveAt") mutation_kind = StateMutationKind::list_remove_at;
        else if (id == "state.listToggle") mutation_kind = StateMutationKind::list_toggle;
        else if (id == "state.listClear") mutation_kind = StateMutationKind::list_clear;
        else if (id == "state.recordSet") mutation_kind = StateMutationKind::record_set;
        else return framework_failure(action, "Unknown framework state action '" + id + "'.");

        std::optional<std::string> undo_label;
        std::optional<std::string> undo_coalesce;
        std::vector<std::pair<std::string, Value>> arguments;
        if (const ValueObject* payload = action.payload.object()) {
            for (const auto& [argument_name, value] : payload->fields) {
                if (argument_name == "undoLabel" && value.string() != nullptr) {
                    undo_label = *value.string();
                } else if (argument_name == "undoCoalesce" && value.string() != nullptr) {
                    undo_coalesce = *value.string();
                } else if (argument_name != "name") {
                    arguments.emplace_back(argument_name, value);
                }
            }
        }
        if (id == "state.setFromEvent") {
            const auto value = std::ranges::find(arguments, "value", &decltype(arguments)::value_type::first);
            if (value != arguments.end()) arguments.erase(value);
            arguments.emplace_back("value", event.value);
        }
        const StateSnapshot before = state_.snapshot();
        const StateMutationResult result = StateMutation{
            resolved->address,
            *initial,
            state_schema(declaration->type_id),
            mutation_kind,
            std::move(arguments),
            std::string(declaration_scope),
        }.apply(state_);
        if (result.status == StateMutationStatus::rejected) return framework_failure(action, result.message);
        if (result.status == StateMutationStatus::changed) {
            if (declaration->persistence_key.has_value()) {
                if (const Value* persisted = state_.find(resolved->address); persisted != nullptr) {
                    durability_.set_application_value(*declaration->persistence_key, *persisted);
                } else {
                    static_cast<void>(durability_.erase_application_value(*declaration->persistence_key));
                }
            }
            if (undo_label.has_value()) {
                undo_.invalidate_other_scopes(effective_undo_scope, id);
                undo_.record(
                    effective_undo_scope,
                    before,
                    state_.snapshot(),
                    UndoRecordOptions{*undo_label, std::move(undo_coalesce), timestamp_nanos}
                );
            } else {
                undo_.invalidate_other_scopes(effective_undo_scope, id);
                undo_.invalidate(effective_undo_scope, id);
            }
        }
        return ActionDispatchOutcome{
            result.status == StateMutationStatus::changed
                ? ActionDispatchStatus::handled
                : ActionDispatchStatus::ignored,
            action.id(),
            {id == "state.setFromEvent" ? "strata.surface.declarative" : "strata.surface.state"},
            std::nullopt,
        };
    }

    if (id == "layer.pop" || id == "layer.push" || id == "layer.replace" ||
        id == "overlay.show" || id == "overlay.hide") {
        DeclarativeLayerOperation operation;
        std::optional<std::string_view> name;
        std::optional<std::string_view> transition;
        if (id == "layer.pop") {
            operation = DeclarativeLayerOperation::pop;
        } else {
            const Value* name_value = action.payload.field("name");
            const std::string* name_string = name_value != nullptr ? name_value->string() : nullptr;
            if (name_string == nullptr) return framework_failure(action, "Layer action requires a name.");
            name = *name_string;
            if (id == "layer.push") operation = DeclarativeLayerOperation::push;
            else if (id == "layer.replace") operation = DeclarativeLayerOperation::replace;
            else if (id == "overlay.show") operation = DeclarativeLayerOperation::show;
            else operation = DeclarativeLayerOperation::hide;
        }
        if (const Value* transition_value = action.payload.field("transition");
            transition_value != nullptr) {
            if (transition_value->string() != nullptr) transition = *transition_value->string();
            else if (transition_value->key() != nullptr) transition = transition_value->key()->value;
        }
        const LayerOperationResult result = layer_registry_.execute(
            layers_, operation, name, transition
        );
        return ActionDispatchOutcome{
            result.status == LayerOperationStatus::handled
                ? ActionDispatchStatus::handled
                : result.status == LayerOperationStatus::ignored
                    ? ActionDispatchStatus::ignored
                    : ActionDispatchStatus::failed,
            action.id(),
            {"strata.surface.declarative"},
            result.message,
        };
    }
    return framework_failure(action, "Framework action '" + id + "' is not implemented by this runtime phase.");
}

std::optional<Value> ApplicationContext::state_initial_value(
    const StateScopeResolution& resolution,
    const std::string_view runtime_scope
) const {
    if (resolution.declaration == nullptr) return std::nullopt;
    if (const Value* initial = state_.initial(resolution.address); initial != nullptr) {
        return *initial;
    }
    ExpressionScope expression_scope;
    expression_scope.component_path = std::string(runtime_scope);
    ExpressionRuntime expressions(host_, bundle_->action_registry(), std::move(expression_scope));
    const ExpressionValue evaluated = expressions.evaluate(resolution.declaration->initializer);
    const Value* value = evaluated.value();
    return value != nullptr && expressions.diagnostics().empty()
               ? std::optional<Value>(*value)
               : std::nullopt;
}
const DeclarativeLayerRegistry& ApplicationContext::layer_registry() const noexcept {
    return layer_registry_;
}
const std::shared_ptr<const RuntimeUnit>& ApplicationContext::active_unit() const noexcept {
    return active_unit_;
}
std::optional<std::uint64_t> ApplicationContext::active_generation() const noexcept {
    return active_generation_;
}
std::optional<std::uint64_t> ApplicationContext::last_attempted_generation() const noexcept {
    return last_attempted_generation_;
}
const ActivationResult* ApplicationContext::last_activation() const noexcept {
    return last_activation_.has_value() ? &*last_activation_ : nullptr;
}
void ApplicationContext::bind_state_scope(
    std::string runtime_scope,
    std::string state_name,
    std::string declaration_scope,
    std::string address_scope
) {
    if (runtime_scope.empty() || state_name.empty() || declaration_scope.empty() ||
        address_scope.empty() ||
        !core::valid_utf8(runtime_scope) || !core::valid_utf8(state_name) ||
        !core::valid_utf8(declaration_scope) || !core::valid_utf8(address_scope)) {
        throw std::invalid_argument("state scope binding names must be non-empty valid UTF-8");
    }
    const StateAddress key{std::move(runtime_scope), std::move(state_name)};
    const StateScopeBinding binding{std::move(declaration_scope), std::move(address_scope)};
    const auto existing = state_scope_bindings_.find(key);
    if (existing != state_scope_bindings_.end() &&
        (existing->second.declaration_scope != binding.declaration_scope ||
         existing->second.address_scope != binding.address_scope)) {
        throw std::invalid_argument("runtime state scope has conflicting declaration owners");
    }
    if (existing == state_scope_bindings_.end()) {
        state_scope_bindings_.emplace(std::move(key), std::move(binding));
    }
}

void ApplicationContext::clear_state_scope_bindings() noexcept { state_scope_bindings_.clear(); }

std::optional<StateScopeResolution> ApplicationContext::resolve_state_scope(
    const std::string_view runtime_scope,
    const std::string_view state_name
) const {
    const auto found = state_scope_bindings_.find(StateAddress{
        std::string(runtime_scope),
        std::string(state_name),
    });
    if (found == state_scope_bindings_.end() || active_unit_ == nullptr) return std::nullopt;
    const UnitStateDeclaration* declaration = active_unit_->state_declaration(
        found->second.declaration_scope,
        state_name
    );
    if (declaration == nullptr) return std::nullopt;
    return StateScopeResolution{
        StateAddress{found->second.address_scope, std::string(state_name)},
        found->second.declaration_scope,
        declaration,
    };
}

ActivationResult ApplicationContext::activate(
    RuntimeUnit::PortableIrStorage portable_ir,
    const std::uint64_t generation,
    compiler::CompiledSourceMap source_map
) {
    auto activation_profile = profiler_.section("activation");
    const auto reject = [this, generation](
                            const ActivationStatus status,
                            std::vector<ActivationDiagnostic> diagnostics
                        ) {
        last_attempted_generation_ = generation;
        last_activation_ = ActivationResult{
            status,
            generation,
            active_generation_,
            false,
            std::move(diagnostics),
        };
        return *last_activation_;
    };
    if (last_attempted_generation_.has_value() && generation <= *last_attempted_generation_) {
        last_activation_ = ActivationResult{
            ActivationStatus::rejected_generation,
            generation,
            active_generation_,
            false,
            {ActivationDiagnostic{
                "STRATA.RUNTIME.ACTIVATION_GENERATION",
                "Activation generations must be strictly increasing.",
                std::nullopt,
                std::nullopt,
            }},
        };
        return *last_activation_;
    }

    std::shared_ptr<const RuntimeUnit> candidate;
    try {
        candidate = RuntimeUnit::create(std::move(portable_ir), std::move(source_map));
    } catch (const std::exception& error) {
        return reject(
            ActivationStatus::rejected_unit,
            {ActivationDiagnostic{
                "STRATA.RUNTIME.INVALID_PORTABLE_IR",
                error.what(),
                std::nullopt,
                std::nullopt,
            }}
        );
    }

    std::vector<std::shared_ptr<const ActionContract>> referenced_contracts;
    for (const std::string& id : candidate->referenced_actions()) {
        const std::shared_ptr<const ActionContract> contract = bundle_->action_registry().contract(id);
        if (contract == nullptr) {
            return reject(
                ActivationStatus::rejected_capability,
                {ActivationDiagnostic{
                    "STRATA.RUNTIME.ACTION_CONTRACT_MISSING",
                    "Portable IR references action '" + id + "' without an active contract.",
                    candidate->source_id(),
                    std::nullopt,
                }}
            );
        }
        referenced_contracts.push_back(contract);
    }
    const auto missing_handlers = actions_.missing_required_handlers(referenced_contracts);
    if (!missing_handlers.empty()) {
        std::vector<ActivationDiagnostic> diagnostics;
        diagnostics.reserve(missing_handlers.size());
        for (const auto& contract : missing_handlers) {
            const UnitActionReference* reference = nullptr;
            for (const UnitActionReference& candidate_reference : candidate->action_references()) {
                if (candidate_reference.id == contract->id) {
                    reference = &candidate_reference;
                    break;
                }
            }
            diagnostics.push_back(ActivationDiagnostic{
                "STRATA.DSL.ACTION_HANDLER_MISSING",
                "Required action '" + contract->id +
                    "' has no active host handler; the module was not activated.",
                reference != nullptr
                    ? std::optional(reference->range.source_id)
                    : std::optional(candidate->source_id()),
                reference != nullptr
                    ? std::optional(reference->component_path)
                    : std::nullopt,
                "one handler registered by the owning surface, layer, or extension",
                DiagnosticSeverity::error,
                reference != nullptr
                    ? std::optional(reference->range)
                    : std::nullopt,
            });
        }
        return reject(ActivationStatus::rejected_capability, std::move(diagnostics));
    }

    DeclarativeLayerRegistry next_layers;
    std::set<std::string, std::less<>> valid_layer_ids;
    for (const std::string& name : candidate->screens()) {
        const std::string id = "screen:" + name;
        next_layers.register_screen(name, id);
        valid_layer_ids.insert(id);
    }
    for (const std::string& name : candidate->overlays()) {
        const std::string overlay_id = "overlay:" + name;
        next_layers.register_overlay(name, overlay_id);
        valid_layer_ids.insert(overlay_id);
        if (!candidate->screen(name)) {
            const std::string screen_id = "screen:" + name;
            next_layers.register_screen(name, screen_id);
            valid_layer_ids.insert(screen_id);
        }
    }
    std::vector<StateDeclarationSchema> declarations;
    declarations.reserve(candidate->state_declarations().size());
    for (const UnitStateDeclaration& declaration : candidate->state_declarations()) {
        declarations.push_back(StateDeclarationSchema{
            declaration.scope,
            declaration.name,
            declaration.type_id,
        });
    }

    batching_invalidations_ = true;
    batched_invalidation_ = false;
    const bool state_migrated = state_.migrate_declarations(declarations);
    undo_.clear_all();
    static_cast<void>(layers_.retain(valid_layer_ids));
    layer_registry_ = std::move(next_layers);
    state_scope_bindings_.clear();
    active_unit_ = std::move(candidate);
    active_generation_ = generation;
    last_attempted_generation_ = generation;
    invalidate();
    batching_invalidations_ = false;
    if (batched_invalidation_) {
        batched_invalidation_ = false;
        invalidate();
    }
    last_activation_ = ActivationResult{
        ActivationStatus::activated,
        generation,
        active_generation_,
        state_migrated,
        {},
    };
    return *last_activation_;
}

ActivationResult ApplicationContext::compile_and_activate(
    const compiler::ModuleSource& entry,
    const compiler::ModuleLoader& loader,
    const std::uint64_t generation,
    std::pmr::memory_resource* const scratch
) {
    if (last_attempted_generation_.has_value() && generation <= *last_attempted_generation_) {
        last_activation_ = ActivationResult{
            ActivationStatus::rejected_generation,
            generation,
            active_generation_,
            false,
            {ActivationDiagnostic{
                "STRATA.RUNTIME.ACTIVATION_GENERATION",
                "Activation generations must be strictly increasing.",
                std::nullopt,
                std::nullopt,
            }},
        };
        return *last_activation_;
    }
    compiler::CompileResult compiled;
    {
        auto compile_profile = profiler_.section("compile");
        compiled = compiler::compile_program(
            entry,
            loader,
            bundle_->schema_registry(),
            scratch
        );
    }
    if (!compiled.succeeded()) {
        std::vector<ActivationDiagnostic> diagnostics;
        diagnostics.reserve(compiled.diagnostics.size());
        for (const compiler::Diagnostic& value : compiled.diagnostics) {
            diagnostics.push_back(activation_diagnostic(value));
        }
        last_attempted_generation_ = generation;
        last_activation_ = ActivationResult{
            ActivationStatus::rejected_compile,
            generation,
            active_generation_,
            false,
            std::move(diagnostics),
        };
        return *last_activation_;
    }
    return activate(
        std::move(*compiled.unit),
        generation,
        std::move(compiled.source_map)
    );
}
std::uint64_t ApplicationContext::dirty_generation() const noexcept { return dirty_generation_; }

void ApplicationContext::invalidate() {
    if (batching_invalidations_) {
        batched_invalidation_ = true;
        return;
    }
    if (dirty_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("application dirty generation exhausted");
    }
    ++dirty_generation_;
}

} // namespace strata::runtime
