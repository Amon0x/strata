#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "compiler/artifact.hpp"
#include "compiler/parser.hpp"
#include "compiler/portable_ir.hpp"
#include "compiler/semantic.hpp"
#include "data/json.hpp"
#include "runtime/action.hpp"
#include "runtime/application.hpp"
#include "runtime/async.hpp"
#include "runtime/durability.hpp"
#include "runtime/expression.hpp"
#include "runtime/host.hpp"
#include "runtime/layer.hpp"
#include "runtime/registry.hpp"
#include "runtime/state.hpp"
#include "runtime/services.hpp"
#include "runtime/undo.hpp"
#include "runtime/value.hpp"
#include "runtime/value_schema.hpp"
#include "resource/resource.hpp"
#include "ui/description.hpp"
#include "ui/tree.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Exception, typename Operation>
void check_throws(Operation&& operation, const std::string_view message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void test_immutable_values_and_runtime_schemas() {
    using namespace strata::runtime;
    const Value record(std::vector<std::pair<std::string, Value>>{
        {"title", Value("Strata")},
        {"enabled", Value(true)},
        {"items", Value(std::vector<Value>{Value(1.0), Value(2.0)})},
    });
    check(record.field("title") != nullptr && *record.field("title")->string() == "Strata", "object field lookup failed");
    check(record.field("missing") == nullptr, "missing object field unexpectedly resolved");
    const Value copy = strata::runtime::value_from_json(strata::runtime::value_to_json(record));
    check(copy == record, "runtime value JSON round trip changed the value");
    check_throws<std::invalid_argument>([] {
        static_cast<void>(Value(std::numeric_limits<double>::infinity()));
    }, "non-finite runtime value was accepted");
    check_throws<std::invalid_argument>([] {
        static_cast<void>(Value(std::vector<std::pair<std::string, Value>>{
            {"duplicate", Value(1.0)},
            {"duplicate", Value(2.0)},
        }));
    }, "duplicate runtime object field was accepted");

    const ValueSchemaPtr string_schema = ValueSchema::scalar(ValueSchemaKind::string);
    const ValueSchemaPtr list_schema = ValueSchema::list(string_schema, false, 2U);
    check(list_schema->accepts(Value(std::vector<Value>{Value("a"), Value("b")})), "valid list schema value rejected");
    check(!list_schema->accepts(Value(std::vector<Value>{Value("a"), Value("b"), Value("c")})), "list maximum was ignored");
    const ValueSchemaPtr object_schema = ValueSchema::object({
        ValueSchemaField{"name", string_schema, true, false},
        ValueSchemaField{"enabled", ValueSchema::scalar(ValueSchemaKind::boolean), false, false},
    });
    check(object_schema->accepts(Value(std::vector<std::pair<std::string, Value>>{{"name", Value("ok")}})), "valid object schema value rejected");
    check(!object_schema->accepts(Value(std::vector<std::pair<std::string, Value>>{{"enabled", Value(true)}})), "required object field was ignored");
}

void test_host_snapshots_are_lazy_and_generation_invalidated() {
    using namespace strata::runtime;
    const strata::data::JsonValue first_json = strata::data::parse_json(
        R"({"app":{"editor":"seed","generation":10,"title":"Initial"},"unused":{"deep":"cold"}})"
    );
    const auto first = HostSnapshot::from_json("initial", 10U, first_json);
    check(first->evaluated_scalar_count() == 0U, "host snapshot eagerly encoded scalar fields");
    const auto title = first->resolve("app.title");
    check(title.has_value() && title->string() != nullptr && *title->string() == "Initial", "host title did not resolve");
    check(first->evaluated_scalar_count() == 1U, "host field resolution evaluated a sibling");
    static_cast<void>(first->resolve("app.title"));
    check(first->evaluated_scalar_count() == 1U, "host field memoization missed");
    static_cast<void>(first->resolve("app.generation"));
    check(first->evaluated_scalar_count() == 2U, "second host field did not evaluate exactly once");
    check(!first->resolve("app.missing").has_value(), "missing host field unexpectedly resolved");

    const auto indexed = HostSnapshot::from_json(
        "indexed",
        1U,
        strata::data::parse_json(
            R"({"app":{"rows":[{"label":"zero","unused":"cold"},{"label":null}],"sibling":"cold"}})"
        )
    );
    const std::vector<HostPathSegment> indexed_label{
        HostPathSegment::named("app"),
        HostPathSegment::named("rows"),
        HostPathSegment::lookup("0", 0U),
        HostPathSegment::named("label"),
    };
    const std::optional<Value> resolved_indexed_label = indexed->resolve(indexed_label);
    check(
        resolved_indexed_label.has_value() && resolved_indexed_label->string() != nullptr &&
            *resolved_indexed_label->string() == "zero" &&
            indexed->evaluated_scalar_count() == 1U,
        "structural host path resolution evaluated an unrelated indexed sibling"
    );
    const std::vector<HostPathSegment> explicit_null{
        HostPathSegment::named("app"),
        HostPathSegment::named("rows"),
        HostPathSegment::lookup("1", 1U),
        HostPathSegment::named("label"),
    };
    const std::vector<HostPathSegment> missing_indexed_field{
        HostPathSegment::named("app"),
        HostPathSegment::named("rows"),
        HostPathSegment::lookup("1", 1U),
        HostPathSegment::named("missing"),
    };
    check(
        indexed->resolve(explicit_null).has_value() &&
            indexed->resolve(explicit_null)->kind() == ValueKind::null_value &&
            !indexed->resolve(missing_indexed_field).has_value() &&
            indexed->evaluated_scalar_count() == 1U,
        "structural host path resolution collapsed explicit null into absence"
    );

    std::vector<std::uint64_t> invalidated;
    HostStore store([&invalidated](const std::uint64_t generation) { invalidated.push_back(generation); });
    check(store.adopt(first), "initial host snapshot was not adopted");
    check(!store.adopt(first), "same host snapshot caused duplicate invalidation");
    check_throws<std::invalid_argument>([&store, &first_json] {
        static_cast<void>(store.adopt(HostSnapshot::from_json("initial", 10U, first_json)));
    }, "repeated per-id host generation was accepted");
    const auto second = HostSnapshot::from_json(
        "updated",
        11U,
        strata::data::parse_json(R"({"app":{"title":"Updated"}})")
    );
    check(store.adopt(second), "new host snapshot was not adopted");
    check(invalidated == std::vector<std::uint64_t>{10U, 11U}, "host invalidation generations changed");
    check(second->evaluated_scalar_count() == 0U, "new host snapshot evaluated before access");
    check(*store.resolve("app.title")->string() == "Updated", "host store did not expose adopted generation");
    const auto data = HostSnapshot::from_json(
        "data", 12U, strata::data::parse_json(R"({"data":{"count":5000}})")
    );
    check(
        store.adopt(data) && *store.resolve("app.title")->string() == "Updated" &&
            *store.resolve("data.count")->number() == 5000.0 && store.snapshots().size() == 3U,
        "independent host roots did not compose across immutable snapshot ids"
    );
}

void test_state_store_and_typed_mutations() {
    using namespace strata::runtime;
    std::uint64_t invalidations = 0U;
    StateStore state([&invalidations] { ++invalidations; });
    const StateAddress count_address{"component/Main", "count"};
    const StateSlot count_slot = StateSlot::from_initial("count", Value(1.0));
    check(*state.read(count_address, count_slot).number() == 1.0, "state initial value changed");
    check(state.generation() == 0U, "reading initial state invalidated the surface");
    check(state.write(count_address, count_slot, Value(2.0)), "state update was not recorded");
    check(!state.write(count_address, count_slot, Value(2.0)), "equal state update invalidated the surface");
    check(state.generation() == 1U && invalidations == 1U, "state invalidation count changed");
    check(
        !state.migrate_declarations({StateDeclarationSchema{
            count_address.scope, count_address.name, "dsl.unknown",
        }}) && state.find(count_address) != nullptr,
        "dynamically inferred state was discarded by declaration migration"
    );

    const StateMutation adjust{
        count_address,
        Value(1.0),
        ValueSchema::scalar(ValueSchemaKind::number),
        StateMutationKind::adjust,
        {{"amount", Value(5.0)}},
    };
    check(adjust.apply(state).status == StateMutationStatus::changed, "numeric state adjustment failed");
    check(*state.find(count_address)->number() == 7.0, "numeric state adjustment value changed");

    const ValueSchemaPtr string_list = ValueSchema::list(
        ValueSchema::scalar(ValueSchemaKind::string),
        false,
        3U
    );
    const StateAddress items_address{"component/Main", "items"};
    const Value initial_items(std::vector<Value>{Value("alpha")});
    const StateMutation append{
        items_address,
        initial_items,
        string_list,
        StateMutationKind::list_append,
        {{"value", Value("beta")}},
    };
    check(append.apply(state).status == StateMutationStatus::changed, "list append failed");
    const StateMutation wrong_element{
        items_address,
        initial_items,
        string_list,
        StateMutationKind::list_append,
        {{"value", Value(3.0)}},
    };
    check(wrong_element.apply(state).status == StateMutationStatus::rejected, "wrong list element type was accepted");

    const ValueSchemaPtr record_schema = ValueSchema::object({
        ValueSchemaField{"status", ValueSchema::scalar(ValueSchemaKind::string), true, false},
    });
    const StateAddress record_address{"component/Main", "record"};
    const Value initial_record(std::vector<std::pair<std::string, Value>>{{"status", Value("initial")}});
    const StateMutation record_set{
        record_address,
        initial_record,
        record_schema,
        StateMutationKind::record_set,
        {{"field", Value("status")}, {"value", Value("updated")}},
    };
    check(record_set.apply(state).status == StateMutationStatus::changed, "record field mutation failed");
    check(*state.find(record_address)->field("status")->string() == "updated", "record mutation value changed");

    const StateSnapshot snapshot = state.snapshot();
    check(state.reset(count_address), "state reset did not remove the slot");
    check(state.restore(snapshot), "state snapshot restore was ignored");
    check(*state.find(count_address)->number() == 7.0, "state snapshot restore changed the value");

    state.mark_owned_scope("component/Main");
    check(state.retain_owned_scopes({}), "detached owned state was not removed");
    check(state.size() == 0U, "detached scope retained state entries");
}

std::shared_ptr<const strata::runtime::ActionContract> contract(
    std::string id,
    const strata::runtime::ActionDispatchPolicy policy
) {
    return std::make_shared<const strata::runtime::ActionContract>(strata::runtime::ActionContract{
        std::move(id),
        strata::runtime::ValueSchema::scalar(strata::runtime::ValueSchemaKind::string),
        policy,
        "string payload",
        "runtime action test",
    });
}

void test_action_contracts_dispatch_and_handler_lifetime() {
    using namespace strata::runtime;
    const auto required = contract("app.save", ActionDispatchPolicy::required);
    const auto optional = contract("app.observe", ActionDispatchPolicy::optional);
    const auto broadcast = contract("app.broadcast", ActionDispatchPolicy::broadcast);
    const auto forwarded = contract("app.forward", ActionDispatchPolicy::forwarded);
    ActionDispatcher dispatcher({required, optional, broadcast, forwarded});
    const ActionEvent event{"activate", "save", Value{}};

    check(
        dispatcher.dispatch(event, Action(required, Value("draft"))).status == ActionDispatchStatus::unhandled,
        "required action without a handler was not unhandled"
    );
    check(
        dispatcher.dispatch(event, Action(optional, Value("notice"))).status == ActionDispatchStatus::ignored,
        "optional action without a handler was not ignored"
    );

    std::vector<std::string> observed;
    auto required_registration = dispatcher.register_handler(
        required,
        "settings",
        [&observed](const ActionContext& context) {
            observed.push_back(*context.action.payload.string());
            return ActionHandlerResult::handled;
        }
    );
    check(
        dispatcher.dispatch(event, Action(required, Value("saved"))).status == ActionDispatchStatus::handled &&
            observed == std::vector<std::string>{"saved"},
        "required action handler did not run"
    );
    check_throws<std::invalid_argument>([&dispatcher, &required] {
        static_cast<void>(dispatcher.register_handler(
            required,
            "duplicate",
            [](const ActionContext&) { return ActionHandlerResult::handled; }
        ));
    }, "duplicate non-broadcast action handler was accepted");

    auto first_broadcast = dispatcher.register_handler(
        broadcast,
        "first",
        [](const ActionContext&) { return ActionHandlerResult::ignored; }
    );
    auto second_broadcast = dispatcher.register_handler(
        broadcast,
        "second",
        [](const ActionContext&) { return ActionHandlerResult::handled; }
    );
    const auto broadcast_outcome = dispatcher.dispatch(event, Action(broadcast, Value("value")));
    check(
        broadcast_outcome.status == ActionDispatchStatus::handled &&
            broadcast_outcome.handler_owners == std::vector<std::string>{"first", "second"},
        "broadcast action aggregation changed"
    );

    auto forwarding_registration = dispatcher.register_handler(
        forwarded,
        "bridge",
        [](const ActionContext&) { return ActionHandlerResult::handled; }
    );
    check(
        dispatcher.dispatch(event, Action(forwarded, Value("value"))).status == ActionDispatchStatus::failed,
        "forwarded action succeeded without crossing the boundary"
    );
    forwarding_registration.close();
    forwarding_registration = dispatcher.register_handler(
        forwarded,
        "bridge",
        [](const ActionContext&) { return ActionHandlerResult::forwarded; }
    );
    check(
        dispatcher.dispatch(event, Action(forwarded, Value("value"))).status == ActionDispatchStatus::forwarded,
        "forwarded action did not report its boundary crossing"
    );

    required_registration.close();
    check(dispatcher.handler_owners("app.save").empty(), "closed action registration retained its handler");
    check(dispatcher.missing_required_handlers({required, forwarded}).size() == 1U, "required-handler audit changed");
}

void test_neutral_registry_projects_runtime_action_contracts(const std::filesystem::path& path) {
    const std::string document = strata::resource::load_utf8_resource(
        path.parent_path(),
        strata::resource::ResourceId::parse(path.filename().string())
    );
    const strata::compiler::SchemaRegistry schema = strata::compiler::SchemaRegistry::parse(
        strata::data::parse_json(document)
    );
    const strata::runtime::RuntimeActionRegistry actions =
        strata::runtime::RuntimeActionRegistry::from_schema(schema);
    check(actions.contracts().size() == 54U, "neutral registry action inventory changed");
    const auto button = actions.contract("Button");
    check(
        button != nullptr && button->dispatch_policy == strata::runtime::ActionDispatchPolicy::optional &&
            button->payload_schema->accepts(strata::runtime::Value{}),
        "built-in observation action contract projection changed"
    );
    const auto state_set = actions.contract("state.set");
    check(
        state_set != nullptr &&
            state_set->dispatch_policy == strata::runtime::ActionDispatchPolicy::framework,
        "framework action dispatch policy projection changed"
    );
    const strata::runtime::Value valid_payload(std::vector<std::pair<std::string, strata::runtime::Value>>{
        {"name", strata::runtime::Value("count")},
        {"value", strata::runtime::Value(3.0)},
    });
    check(state_set->payload_schema->accepts(valid_payload), "framework action payload schema rejected valid arguments");
    check(
        !state_set->payload_schema->accepts(strata::runtime::Value(
            std::vector<std::pair<std::string, strata::runtime::Value>>{
                {"name", strata::runtime::Value("count")},
            }
        )),
        "framework action payload schema ignored a required argument"
    );
}

void test_animation_validation_is_total(const std::filesystem::path& path) {
    using namespace strata;
    const std::string document = resource::load_utf8_resource(
        path.parent_path(),
        resource::ResourceId::parse(path.filename().string())
    );
    const compiler::SchemaRegistry schema = compiler::SchemaRegistry::parse(
        data::parse_json(document)
    );
    const std::string invalid_source = R"(
animation DuplicateFrame {
  from { opacity: 0 }
  from { opacity: 0.5 }
  to { opacity: 1 }
  to { opacity: 0.75 }
}
animation MismatchedFrames {
  from { opacity: 0, x: 1 }
  to { opacity: 1 }
}
animation DynamicInputs {
  from { opacity: 0 + 1 }
  to { opacity: 1 }
  duration: 50ms + 50ms;
}
animation InvalidTiming {
  from { opacity: 0 }
  to { opacity: 1 }
  duration: 0ms;
  delay: -1ms;
}
animation FractionalRepeat {
  from { opacity: 0 }
  to { opacity: 1 }
  repeat: 1.5;
}
animation ZeroRepeat {
  from { opacity: 0 }
  to { opacity: 1 }
  repeat: { count: 0 };
}
animation FrozenEnum {
  from { opacity: 0 }
  to { opacity: 1 }
  fillMode: "forwards";
}
)";
    const compiler::ParseResult parsed = compiler::parse_source(
        "animation-invalid.strata",
        invalid_source
    );
    check(parsed.diagnostics.empty(), "animation validation fixture did not parse");
    const compiler::SemanticResult invalid = compiler::validate_semantics(parsed.file, schema);
    const auto has_code = [&invalid](const std::string_view code) {
        return std::ranges::any_of(invalid.diagnostics, [code](const compiler::Diagnostic& value) {
            return value.code == code;
        });
    };
    check(
        std::ranges::count(
            invalid.diagnostics,
            std::string_view("STRATA.DSL.SEMANTIC_DUPLICATE_ANIMATION_FRAME"),
            &compiler::Diagnostic::code
        ) == 2U &&
            has_code("STRATA.DSL.SEMANTIC_ANIMATION_FRAME_MISMATCH") &&
            has_code("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED") &&
            has_code("STRATA.DSL.SEMANTIC_ANIMATION_DURATION_BOUNDS") &&
            has_code("STRATA.DSL.SEMANTIC_ANIMATION_REPEAT_COUNT") &&
            has_code("STRATA.DSL.SEMANTIC_ANIMATION_SCHEMA_VALUE") &&
            invalid.animations.empty(),
        "animation semantic validation did not reject every malformed lowering contract"
    );
    check(
        std::ranges::all_of(invalid.diagnostics, [&invalid_source](const compiler::Diagnostic& value) {
            return value.range.has_value() &&
                   value.range->source_id == "animation-invalid.strata" &&
                   value.range->start.offset <= value.range->end.offset &&
                   value.range->end.offset <= invalid_source.size();
        }),
        "animation validation diagnostic lost its authored source range"
    );

    const std::string valid_source = R"(
animation Validated {
  from { opacity: 0, translateY: -9 }
  to { opacity: 1, translateY: 0 }
  duration: 125ms;
  delay: 0ms;
  easing: "ease-in";
  fillMode: "BACKWARDS";
  repeat: { count: 2 };
  reverse: true;
  trigger: "ENTER";
}
)";
    const compiler::ParseResult valid_parsed = compiler::parse_source(
        "animation-valid.strata",
        valid_source
    );
    check(valid_parsed.diagnostics.empty(), "validated animation fixture did not parse");
    const compiler::SemanticResult valid = compiler::validate_semantics(valid_parsed.file, schema);
    check(
        valid.diagnostics.empty() && valid.lowering_diagnostics.empty() &&
            valid.animations.size() == 1U,
        "valid animation did not produce exactly one typed lowering payload"
    );
    const compiler::PortableIrResult lowered = compiler::lower_portable_ir(
        valid_parsed.file,
        schema,
        valid.animations
    );
    const data::JsonValue* declarations = lowered.unit.has_value()
                                                   ? lowered.unit->find("animations")
                                                   : nullptr;
    const data::JsonValue* declaration = declarations != nullptr &&
                                                 declarations->array() != nullptr &&
                                                 !declarations->array()->empty()
                                             ? &declarations->array()->front()
                                             : nullptr;
    const data::JsonValue* animation = declaration != nullptr
                                           ? declaration->find("animation")
                                           : nullptr;
    const data::JsonValue* timing = animation != nullptr ? animation->find("timing") : nullptr;
    const data::JsonValue* repeat = timing != nullptr ? timing->find("repeat") : nullptr;
    check(
        lowered.diagnostics.empty() && animation != nullptr && timing != nullptr &&
            *timing->find("durationNanos")->integer() == 125'000'000 &&
            *timing->find("delayNanos")->integer() == 0 &&
            *timing->find("fillMode")->string() == "backwards" &&
            repeat != nullptr && *repeat->find("kind")->string() == "count" &&
            *repeat->find("count")->integer() == 2 &&
            declaration->find("trigger") != nullptr &&
            *declaration->find("trigger")->string() == "ENTER",
        "portable animation lowering did not project the typed validated timing payload exactly"
    );
    const data::JsonValue* tracks = animation->find("tracks");
    const data::JsonValue* translated = nullptr;
    if (tracks != nullptr && tracks->array() != nullptr) {
        for (const data::JsonValue& track : *tracks->array()) {
            const data::JsonValue* property = track.find("property");
            if (property != nullptr && property->string() != nullptr &&
                *property->string() == "translateY") {
                translated = &track;
                break;
            }
        }
    }
    check(
        translated != nullptr && translated->find("keyframes") != nullptr &&
            translated->find("keyframes")->array() != nullptr &&
            *translated->find("keyframes")->array()->front().find("value")->find("value")->number() == -9.0,
        "signed literal animation track value was defaulted or normalized during lowering"
    );
}

void test_application_bundle_and_runtime_isolation(const std::filesystem::path& path) {
    const std::string document = strata::resource::load_utf8_resource(
        path.parent_path(),
        strata::resource::ResourceId::parse(path.filename().string())
    );
    const auto bundle = strata::runtime::ApplicationBundle::create(strata::data::parse_json(document));
    strata::runtime::ApplicationContext left("left", bundle);
    strata::runtime::ApplicationContext right("right", bundle);
    const strata::runtime::StateAddress address{"component/Main", "count"};
    const strata::runtime::StateSlot slot = strata::runtime::StateSlot::from_initial(
        "count",
        strata::runtime::Value(0.0)
    );
    static_cast<void>(left.state().read(address, slot));
    static_cast<void>(right.state().read(address, slot));
    check(left.state().write(address, slot, strata::runtime::Value(1.0)), "left application state update failed");
    check(
        *left.state().find(address)->number() == 1.0 &&
            *right.state().find(address)->number() == 0.0,
        "application contexts shared retained state"
    );
    const auto left_host = strata::runtime::HostSnapshot::from_json(
        "left-host",
        1U,
        strata::data::parse_json(R"({"app":{"title":"Left"}})")
    );
    static_cast<void>(left.host().adopt(left_host));
    check(
        left.host().resolve("app.title").has_value() &&
            !right.host().resolve("app.title").has_value(),
        "application contexts shared host snapshots"
    );
    left.layers().push_screen("left-screen");
    check(
        left.layers().active_screen() == "left-screen" && !right.layers().active_screen().has_value(),
        "application contexts shared layer stacks"
    );
    check(
        left.dirty_generation() == 3U && right.dirty_generation() == 0U,
        "application invalidation accounting changed"
    );
}

void test_canonical_runtime_diagnostic_store() {
    using namespace strata::runtime;
    std::vector<RuntimeDiagnosticRecord> published;
    std::vector<std::uint64_t> dropped;
    RuntimeServices services([&](
        const RuntimeDiagnosticRecord& record,
        const std::uint64_t dropped_count
    ) {
        published.push_back(record);
        dropped.push_back(dropped_count);
    });
    const RuntimeDiagnostic diagnostic{
        "STRATA.TEST.CANONICAL_DIAGNOSTIC",
        "Canonical diagnostic transport test.",
        "screen Main/Button.test",
        std::optional<std::string>("a valid test value"),
        DiagnosticSeverity::warning,
        DiagnosticRange{
            "test/main.strata",
            DiagnosticPosition{3U, 5U, 12U},
            DiagnosticPosition{3U, 9U, 16U},
        },
    };
    services.report(diagnostic);
    check(services.begin_frame() == 1U, "diagnostic publication frame did not advance");
    services.end_frame();
    services.report(diagnostic);
    check(services.begin_frame() == 2U, "aggregated diagnostic frame did not advance");
    services.end_frame();
    check(
        published.size() == 2U && published[0].sequence == published[1].sequence &&
            published[1].first_frame_index == 1U && published[1].frame_index == 2U &&
            published[1].occurrence_count == 2U &&
            published[1].diagnostic.path == "screen Main/Button.test" &&
            published[1].diagnostic.expected == "a valid test value" &&
            dropped == std::vector<std::uint64_t>{0U, 0U},
        "canonical diagnostic callback did not preserve aggregation and context"
    );
    services.report(RuntimeDiagnostic{
        "STRATA.TEST.PENDING", "Must be cleared before publication.", {}, std::nullopt,
        DiagnosticSeverity::error, std::nullopt,
    });
    services.clear_diagnostics();
    check(services.begin_frame() == 3U, "diagnostic clear frame did not advance");
    services.end_frame();
    const RuntimeDiagnosticsSnapshot snapshot = services.diagnostics_snapshot();
    check(
        snapshot.records.empty() && snapshot.dropped_count == 0U && published.size() == 2U,
        "diagnostic clear retained or published pending records"
    );
    bool inactive_publication_rejected = false;
    try {
        services.publish_current_frame(diagnostic);
    } catch (const std::logic_error&) {
        inactive_publication_rejected = true;
    }
    check(
        inactive_publication_rejected,
        "current-frame diagnostic publication accepted an inactive runtime frame"
    );
    check(services.begin_frame() == 4U, "current diagnostic frame did not advance");
    services.publish_current_frame(diagnostic);
    services.publish_current_frame(diagnostic);
    check(
        !services.has_pending_frame_work() && published.size() == 4U &&
            published[2U].sequence == published[3U].sequence &&
            published[3U].frame_index == 4U &&
            published[3U].occurrence_count == 2U &&
            services.diagnostics_snapshot().records.size() == 1U,
        "current-frame diagnostic publication bypassed canonical aggregation or queued work"
    );
    services.end_frame();
}

void test_transactional_activation_and_last_good_reload(
    const std::filesystem::path& registry_path,
    const std::filesystem::path& fixture_root
) {
    using namespace strata::runtime;
    const std::filesystem::path scenario_directory = fixture_root;
    const auto load = [](const std::filesystem::path& root, const std::string& name) {
        return strata::resource::load_utf8_resource(
            root,
            strata::resource::ResourceId::parse(name)
        );
    };
    const auto schemas = strata::data::parse_json(load(scenario_directory, "schemas.json"));
    const auto registry = strata::data::parse_json(load(registry_path.parent_path(), registry_path.filename().string()));
    const auto bundle = ApplicationBundle::create(registry, &schemas);
    ApplicationContext application("reload", bundle);
    const auto loader = [](const std::string_view, const std::string_view path) -> strata::compiler::ModuleSource {
        throw strata::compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };

    ApplicationContext inferred_state("inferred-state", bundle);
    const ActivationResult inferred_activation = inferred_state.compile_and_activate(
        strata::compiler::ModuleSource{
            "app/inferred.strata",
            "overlay Inferred { state title = app.title; root Text(text: title) }",
        },
        loader,
        0U
    );
    check(
        inferred_activation.activated() &&
            inferred_state.active_unit()->state_declarations().size() == 1U &&
            !inferred_state.active_unit()->state_declarations().front().declared_type.has_value(),
        "inferred host-backed state did not preserve a nullable declared type"
    );

    ApplicationContext identity_contract("portable-identity-contract", bundle);
    check(
        identity_contract.compile_and_activate(
            strata::compiler::ModuleSource{
                "app/identity.strata",
                "overlay Identity { root Panel() { for item in [1] { Text(text: format(\"{0}\", item)) } } }",
            },
            loader,
            0U
        ).activated(),
        "portable identity contract fixture did not compile"
    );
    const std::string ordinary_ir = strata::data::encode_canonical_json(
        identity_contract.active_unit()->portable_ir()
    );
    check(
        ordinary_ir.find("\"identity\":") == std::string::npos,
        "ordinary for IR emitted a Repeater-only identity field"
    );
    ApplicationContext ordinary_round_trip("portable-ordinary-for-round-trip", bundle);
    check(
        ordinary_round_trip.activate(
            strata::data::materialize_json(identity_contract.active_unit()->portable_ir()),
            0U
        ).activated(),
        "portable IR activation required identity on an ordinary for"
    );

    std::string ordinary_ir_with_extra_identity = ordinary_ir;
    const std::size_t ordinary_index_name = ordinary_ir_with_extra_identity.find(
        "\"indexName\":"
    );
    check(
        ordinary_index_name != std::string::npos,
        "ordinary for IR did not contain its indexName field"
    );
    ordinary_ir_with_extra_identity.insert(
        ordinary_index_name,
        "\"identity\":false,"
    );
    ApplicationContext ordinary_extra_identity("portable-ordinary-for-extra-identity", bundle);
    const ActivationResult extra_identity_result = ordinary_extra_identity.activate(
        strata::data::parse_json(ordinary_ir_with_extra_identity),
        0U
    );
    check(
        extra_identity_result.activated(),
        "portable IR treated an ordinary for identity extension as Repeater runtime data"
    );

    ApplicationContext repeater_identity("portable-repeater-identity", bundle);
    check(
        repeater_identity.compile_and_activate(
            strata::compiler::ModuleSource{
                "app/repeater-identity.strata",
                "overlay RepeaterIdentity { root Repeater(estimatedItemExtent: 24) { for item in [{ key: \"one\" }] { Panel(key: item.key) } } }",
            },
            loader,
            0U
        ).activated(),
        "portable Repeater identity fixture did not compile"
    );
    std::string null_repeater_identity = strata::data::encode_canonical_json(
        repeater_identity.active_unit()->portable_ir()
    );
    constexpr std::string_view identity_object = "\"identity\": {";
    const std::size_t repeater_identity_position = null_repeater_identity.find(identity_object);
    check(
        repeater_identity_position != std::string::npos,
        "compiled Repeater IR did not contain an identity plan"
    );
    const std::size_t identity_start =
        repeater_identity_position + identity_object.size() - 1U;
    std::size_t identity_end = identity_start;
    std::size_t identity_depth = 0U;
    bool in_string = false;
    bool escaped = false;
    for (; identity_end < null_repeater_identity.size(); ++identity_end) {
        const char character = null_repeater_identity[identity_end];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            ++identity_depth;
        } else if (character == '}' && --identity_depth == 0U) {
            ++identity_end;
            break;
        }
    }
    check(identity_depth == 0U, "compiled Repeater identity plan was not a bounded object");
    std::string missing_repeater_identity = null_repeater_identity;
    check(
        identity_end < missing_repeater_identity.size() &&
            missing_repeater_identity[identity_end] == ',',
        "compiled Repeater identity plan was not followed by another loop field"
    );
    missing_repeater_identity.erase(
        repeater_identity_position,
        identity_end - repeater_identity_position + 1U
    );
    ApplicationContext missing_repeater_identity_context(
        "portable-repeater-identity-missing",
        bundle
    );
    const ActivationResult missing_repeater_identity_result =
        missing_repeater_identity_context.activate(
            strata::data::parse_json(missing_repeater_identity),
            0U
        );
    check(
        missing_repeater_identity_result.status == ActivationStatus::rejected_unit &&
            !missing_repeater_identity_result.diagnostics.empty() &&
            missing_repeater_identity_result.diagnostics.front().message.find("identity") !=
                std::string::npos,
        "portable IR activation accepted a Repeater with a missing identity plan"
    );
    null_repeater_identity.replace(
        identity_start,
        identity_end - identity_start,
        "null"
    );
    ApplicationContext malformed_repeater_identity("portable-repeater-identity-null", bundle);
    const ActivationResult null_identity_result = malformed_repeater_identity.activate(
        strata::data::parse_json(null_repeater_identity),
        0U
    );
    check(
        null_identity_result.status == ActivationStatus::rejected_unit &&
            !null_identity_result.diagnostics.empty() &&
            null_identity_result.diagnostics.front().message.find("identity") !=
                std::string::npos,
        "portable IR activation accepted a Repeater with a null identity plan"
    );

    ActivationResult initial = application.compile_and_activate(
        strata::compiler::ModuleSource{"app/main.strata", load(scenario_directory, "main.strata")},
        loader,
        0U
    );
    check(
        initial.activated() && application.active_generation() == 0U,
        "initial runtime unit activation failed (status " +
            std::to_string(static_cast<int>(initial.status)) + "): " +
            (initial.diagnostics.empty() ? std::string("no diagnostic") : initial.diagnostics.front().message)
    );
    check(
        static_cast<bool>(application.active_unit()->overlay("Main")),
        "active overlay was not indexed"
    );

    const strata::data::JsonValue source_portable_ir = strata::data::materialize_json(
        application.active_unit()->portable_ir()
    );
    strata::compiler::CompiledModuleArtifact frozen_artifact =
        strata::compiler::decode_compiled_module_artifact(
            strata::compiler::encode_compiled_module_artifact(
                source_portable_ir,
                application.active_unit()->source_map()
            )
        );
    ApplicationContext frozen_application("compiled-round-trip", bundle);
    const ActivationResult frozen_activation = frozen_application.activate(
        std::move(frozen_artifact.unit),
        0U,
        std::move(frozen_artifact.source_map)
    );
    check(
        frozen_activation.activated() &&
            strata::data::encode_canonical_json(frozen_application.active_unit()->portable_ir()) ==
                strata::data::encode_canonical_json(source_portable_ir) &&
            static_cast<bool>(frozen_application.active_unit()->overlay("Main")),
        "frozen portable IR activation diverged from source-owned activation"
    );

    const StateAddress count_address{
        "dsl:app/main.strata:overlay Main/state:/overlay/Main/body/0",
        "count",
    };
    const StateSlot count_slot = StateSlot::from_initial("count", Value(0.0), "overlay Main");
    const StateAddress modal_address{
        "dsl:app/main.strata:overlay Main/state:/overlay/Main/body/1",
        "modalOpen",
    };
    const StateSlot modal_slot = StateSlot::from_initial("modalOpen", Value(true), "overlay Main");
    static_cast<void>(application.state().read(count_address, count_slot));
    static_cast<void>(application.state().read(modal_address, modal_slot));
    check(application.state().write(count_address, count_slot, Value(4.0)), "pre-reload state write failed");

    ActivationResult good = application.compile_and_activate(
        strata::compiler::ModuleSource{
            "app/main.strata",
            load(scenario_directory, "reload-good.strata"),
        },
        loader,
        1U
    );
    check(good.activated() && good.state_migrated, "compatible reload did not activate and migrate");
    check(
        application.state().find(count_address) != nullptr &&
            *application.state().find(count_address)->number() == 4.0 &&
            application.state().find(modal_address) == nullptr,
        "reload did not preserve compatible state and discard removed declarations"
    );
    const auto adjust_contract = bundle->action_registry().contract("state.adjust");
    const Action adjust(
        adjust_contract,
        Value(std::vector<std::pair<std::string, Value>>{
            {"amount", Value(1.0)},
            {"name", Value("count")},
        })
    );
    application.bind_state_scope(
        "overlay Main",
        "count",
        "overlay Main",
        count_address.scope
    );
    const ActionDispatchOutcome adjusted = application.dispatch(
        ActionEvent{"activate", std::string("dynamic.increment"), Value{}},
        adjust,
        "overlay Main"
    );
    check(
        adjusted.status == ActionDispatchStatus::handled &&
            *application.state().find(count_address)->number() == 5.0,
        "framework state action was not consumed against its declaration scope"
    );
    const auto show_contract = bundle->action_registry().contract("overlay.show");
    const Action show(
        show_contract,
        Value(std::vector<std::pair<std::string, Value>>{{"name", Value("Main")}})
    );
    check(
        application.dispatch(ActionEvent{"activate", std::nullopt, Value{}}, show).status ==
                ActionDispatchStatus::handled &&
            application.layers().snapshot() ==
                std::vector<LayerSnapshot>{{"overlay:Main", LayerRole::overlay}},
        "framework overlay action did not use the active declarative registry"
    );
    const auto last_good = application.active_unit();

    ActivationResult bad = application.compile_and_activate(
        strata::compiler::ModuleSource{
            "app/main.strata",
            load(scenario_directory, "reload-bad.strata"),
        },
        loader,
        2U
    );
    check(
        bad.status == ActivationStatus::rejected_compile && application.active_unit() == last_good &&
            application.active_generation() == 1U,
        "compile failure replaced the last-good runtime unit"
    );

    ActivationResult incompatible = application.compile_and_activate(
        strata::compiler::ModuleSource{
            "app/main.strata",
            load(scenario_directory, "reload-incompatible.strata"),
        },
        loader,
        3U
    );
    check(
        incompatible.status == ActivationStatus::rejected_capability &&
            !incompatible.diagnostics.empty() && application.active_unit() == last_good &&
            application.state().find(count_address) != nullptr,
        "missing required host capability did not preserve last-good unit and state"
    );

    ActivationResult stale = application.activate(
        strata::data::materialize_json(last_good->portable_ir()),
        2U
    );
    check(
        stale.status == ActivationStatus::rejected_generation &&
            application.last_attempted_generation() == 3U && application.active_unit() == last_good,
        "stale activation generation regressed the active application"
    );
}

void test_portable_ir_expression_runtime(const std::filesystem::path& path) {
    using namespace strata::runtime;
    const std::string document = strata::resource::load_utf8_resource(
        path.parent_path(),
        strata::resource::ResourceId::parse(path.filename().string())
    );
    const auto bundle = ApplicationBundle::create(strata::data::parse_json(document));
    HostStore host;
    const auto host_snapshot = HostSnapshot::from_json(
        "expression-host",
        1U,
        strata::data::parse_json(
            R"({"app":{"title":"Lazy title","unused":"cold","items":[]},"other":{"value":"untouched"}})"
        )
    );
    static_cast<void>(host.adopt(host_snapshot));
    ExpressionScope scope;
    scope.component_path = "screen Main";
    scope.values.emplace("items", Value(std::vector<Value>{Value("alpha"), Value("beta")}));
    scope.values.emplace("count", Value(2.0));
    ExpressionRuntime runtime(host, bundle->action_registry(), scope);
    struct DependencyCapture final : ExpressionDependencyObserver {
        void lexical(const std::string_view, const ExpressionDependencyValue&) override {}
        void host(const ExpressionHostDependency& dependency) override {
            reads.push_back(dependency);
        }
        std::vector<ExpressionHostDependency> reads;
    } dependencies;
    static_cast<void>(runtime.exchange_dependency_observer(&dependencies));

    const auto host_expression = strata::data::parse_json(R"({
      "kind":"property","name":"title","path":"/host/title",
      "receiver":{"kind":"variable","binding":"host","name":"app","path":"/host"}
    })");
    const ExpressionValue host_value = runtime.evaluate(host_expression);
    check(
        host_value.value() != nullptr && *host_value.value()->string() == "Lazy title" &&
            host_snapshot->evaluated_scalar_count() == 1U,
        "portable IR host property evaluation was eager or incorrect"
    );
    const auto host_list_is_empty_expression = strata::data::parse_json(R"({
      "kind":"property","name":"isEmpty","path":"/host/items/empty",
      "receiver":{"kind":"property","name":"items","path":"/host/items",
        "receiver":{"kind":"variable","binding":"host","name":"app","path":"/host"}}
    })");
    const ExpressionValue host_list_is_empty = runtime.evaluate(host_list_is_empty_expression);
    check(
        host_list_is_empty.value() != nullptr &&
            host_list_is_empty.value()->boolean() != nullptr &&
            *host_list_is_empty.value()->boolean(),
        "computed host-list property was resolved as a structural host field"
    );
    dependencies.reads.clear();
    const auto indexed_host_expression = strata::data::parse_json(R"({
      "kind":"index","path":"/host/indexed",
      "receiver":{"kind":"variable","binding":"host","name":"app","path":"/host"},
      "index":{"kind":"literal","path":"/host/key","value":{"kind":"string","value":"title"}}
    })");
    const ExpressionValue indexed_host_value = runtime.evaluate(indexed_host_expression);
    check(
        indexed_host_value.value() != nullptr &&
            *indexed_host_value.value()->string() == "Lazy title" &&
            dependencies.reads.size() == 1U &&
            dependencies.reads.front().path.size() == 2U &&
            dependencies.reads.front().path.back().kind == HostPathSegmentKind::lookup,
        "indexed host evaluation did not publish one exact structural leaf read"
    );
    dependencies.reads.clear();
    const auto short_circuit_host_expression = strata::data::parse_json(R"({
      "kind":"binary","operator":"and","path":"/host/short-circuit",
      "left":{"kind":"literal","path":"/false","value":{"kind":"boolean","value":false}},
      "right":{"kind":"index","path":"/host/unread",
        "receiver":{"kind":"variable","binding":"host","name":"app","path":"/host"},
        "index":{"kind":"literal","path":"/host/unused-key","value":{"kind":"string","value":"unused"}}
      }
    })");
    static_cast<void>(runtime.evaluate(short_circuit_host_expression));
    check(
        dependencies.reads.empty() && host_snapshot->evaluated_scalar_count() == 1U,
        "short-circuited host branch was observed or evaluated"
    );
    static_cast<void>(runtime.exchange_dependency_observer(nullptr));

    const auto arithmetic = strata::data::parse_json(R"({
      "kind":"binary","operator":"multiply","path":"/arithmetic",
      "left":{"kind":"variable","binding":"local","name":"count","path":"/count"},
      "right":{"kind":"literal","path":"/two","value":{"kind":"number","value":2}}
    })");
    check(
        *runtime.evaluate(arithmetic).value()->number() == 4.0,
        "portable IR arithmetic evaluation changed"
    );

    const auto mapped = strata::data::parse_json(R"({
      "kind":"helper","name":"map","path":"/mapped","arguments":[
        {"name":null,"value":{"kind":"variable","binding":"local","name":"items","path":"/items"}},
        {"name":null,"value":{"kind":"lambda","parameter":"item","path":"/lambda","body":{
          "kind":"helper","name":"upper","path":"/upper","arguments":[
            {"name":null,"value":{"kind":"variable","binding":"local","name":"item","path":"/item"}}
          ]
        }}}
      ]
    })");
    const ExpressionValue first_mapping = runtime.evaluate(mapped);
    const ExpressionValue second_mapping = runtime.evaluate(mapped);
    check(first_mapping.collection() != nullptr && second_mapping.collection() != nullptr, "collection helper did not return a view");
    check(
        *first_mapping.collection() == *second_mapping.collection() &&
            (*second_mapping.collection())->cache_hits.load(std::memory_order_relaxed) == 1U,
        "derived collection view was not generation-memoized"
    );
    const auto* mapped_items = (*first_mapping.collection())->items.list();
    check(
        mapped_items != nullptr && mapped_items->values.size() == 2U &&
            *mapped_items->values[0].string() == "ALPHA" &&
            *mapped_items->values[1].string() == "BETA",
        "portable IR collection map evaluation changed"
    );

    const auto metadata_inner_map = strata::data::parse_json(R"({
      "kind":"helper","name":"map","path":"/metadata/inner","arguments":[
        {"name":null,"value":{"kind":"variable","binding":"local","name":"view","path":"/metadata/view"}},
        {"name":null,"value":{"kind":"lambda","parameter":"inner","path":"/metadata/inner-lambda","body":{
          "kind":"helper","name":"upper","path":"/metadata/upper","arguments":[
            {"name":null,"value":{"kind":"variable","binding":"local","name":"inner","path":"/metadata/inner-item"}}
          ]
        }}}
      ]
    })");
    const auto nested_metadata_map = strata::data::parse_json(R"({
      "kind":"helper","name":"map","path":"/metadata/outer","arguments":[
        {"name":null,"value":{
          "kind":"helper","name":"map","path":"/metadata/inner","arguments":[
            {"name":null,"value":{"kind":"variable","binding":"local","name":"view","path":"/metadata/view"}},
            {"name":null,"value":{"kind":"lambda","parameter":"inner","path":"/metadata/inner-lambda","body":{
              "kind":"helper","name":"upper","path":"/metadata/upper","arguments":[
                {"name":null,"value":{"kind":"variable","binding":"local","name":"inner","path":"/metadata/inner-item"}}
              ]
            }}}
          ]
        }},
        {"name":null,"value":{"kind":"lambda","parameter":"outer","path":"/metadata/outer-lambda","body":{
          "kind":"variable","binding":"local","name":"outer","path":"/metadata/outer-item"
        }}}
      ]
    })");
    const auto metadata_view = [](const std::size_t total) {
        return std::shared_ptr<const CollectionViewValue>(new CollectionViewValue{
            CollectionViewImmutableIdentity{
                Value(std::vector<Value>{Value("A")}),
                total,
                1U,
                0U,
                1U,
                "window",
            },
        });
    };
    ExpressionScope metadata_scope = scope;
    metadata_scope.executable_values.insert_or_assign(
        "view",
        ExpressionValue(metadata_view(1U))
    );
    static_cast<void>(runtime.evaluate_in(metadata_inner_map, metadata_scope));
    const ExpressionValue metadata_one = runtime.evaluate_in(
        nested_metadata_map,
        metadata_scope
    );
    metadata_scope.executable_values.insert_or_assign(
        "view",
        ExpressionValue(metadata_view(2U))
    );
    const ExpressionValue metadata_two = runtime.evaluate_in(
        nested_metadata_map,
        metadata_scope
    );
    const ExpressionValue metadata_two_hit = runtime.evaluate_in(
        nested_metadata_map,
        metadata_scope
    );
    check(
        metadata_one.collection() != nullptr && metadata_two.collection() != nullptr &&
            metadata_two_hit.collection() != nullptr &&
            (*metadata_one.collection())->items == (*metadata_two.collection())->items &&
            (*metadata_one.collection())->total == 1U &&
            (*metadata_two.collection())->total == 2U &&
            *metadata_one.collection() != *metadata_two.collection() &&
            *metadata_two.collection() == *metadata_two_hit.collection(),
        "nested collection cache collapsed equal items with changed CollectionView metadata"
    );

    const auto stable_metadata_map = strata::data::parse_json(R"({
      "kind":"helper","name":"map","path":"/metadata/stable","arguments":[
        {"name":null,"value":{"kind":"variable","binding":"local","name":"view","path":"/metadata/stable-view"}},
        {"name":null,"value":{"kind":"lambda","parameter":"item","path":"/metadata/stable-lambda","body":{
          "kind":"variable","binding":"local","name":"item","path":"/metadata/stable-item"
        }}}
      ]
    })");
    const std::shared_ptr<const CollectionViewValue> stable_source = metadata_view(2U);
    metadata_scope.executable_values.insert_or_assign(
        "view",
        ExpressionValue(stable_source)
    );
    const ExpressionValue stable_metadata_one = runtime.evaluate_in(
        stable_metadata_map,
        metadata_scope
    );
    static_cast<void>(stable_source->cache_hits.fetch_add(1U, std::memory_order_relaxed));
    const ExpressionValue stable_metadata_hit = runtime.evaluate_in(
        stable_metadata_map,
        metadata_scope
    );
    check(
        stable_metadata_one.collection() != nullptr &&
            stable_metadata_hit.collection() != nullptr &&
            *stable_metadata_one.collection() == *stable_metadata_hit.collection() &&
            (*stable_metadata_hit.collection())
                    ->cache_hits.load(std::memory_order_relaxed) == 1U,
        "volatile CollectionView cache statistics poisoned a stable metadata cache hit"
    );

    const auto traced_filter = strata::data::parse_json(R"({
      "kind":"helper","name":"filter","path":"/traced-filter","arguments":[
        {"name":null,"value":{"kind":"variable","binding":"local","name":"numbers","path":"/numbers"}},
        {"name":null,"value":{"kind":"lambda","parameter":"item","path":"/predicate","body":{
          "kind":"binary","operator":"and","path":"/predicate/guard",
          "left":{"kind":"variable","binding":"local","name":"enabled","path":"/enabled"},
          "right":{"kind":"binary","operator":"greater","path":"/predicate/compare",
            "left":{"kind":"variable","binding":"local","name":"item","path":"/item"},
            "right":{"kind":"index","path":"/host/minimum",
              "receiver":{"kind":"index","path":"/host/limits",
                "receiver":{"kind":"variable","binding":"host","name":"app","path":"/host/app"},
                "index":{"kind":"literal","path":"/host/limits-key","value":{"kind":"string","value":"limits"}}
              },
              "index":{"kind":"literal","path":"/host/minimum-key","value":{"kind":"string","value":"minimum"}}
            }
          }
        }}}
      ]
    })");
    ExpressionScope traced_scope = scope;
    traced_scope.values.insert_or_assign(
        "numbers",
        Value(std::vector<Value>{Value(1.0), Value(2.0), Value(3.0)})
    );
    traced_scope.values.insert_or_assign("enabled", Value(false));
    traced_scope.values.insert_or_assign("unrelated", Value("first"));
    const ExpressionValue guarded = runtime.evaluate_in(traced_filter, traced_scope);
    const auto unrelated_host = HostSnapshot::from_json(
        "expression-host-unrelated",
        2U,
        strata::data::parse_json(
            R"({"app":{"limits":{"minimum":100,"sibling":"cold"},"unrelated":"changed"}})"
        )
    );
    static_cast<void>(host.adopt(unrelated_host));
    traced_scope.values.insert_or_assign("unrelated", Value("second"));
    const ExpressionValue guarded_hit = runtime.evaluate_in(traced_filter, traced_scope);
    check(
        guarded.collection() != nullptr && guarded_hit.collection() != nullptr &&
            *guarded.collection() == *guarded_hit.collection() &&
            unrelated_host->evaluated_scalar_count() == 0U,
        "unrelated lexical/host changes invalidated or evaluated a short-circuited collection dependency"
    );

    traced_scope.values.insert_or_assign("enabled", Value(true));
    const ExpressionValue selected = runtime.evaluate_in(traced_filter, traced_scope);
    check(
        selected.collection() != nullptr && *selected.collection() != *guarded.collection() &&
            unrelated_host->evaluated_scalar_count() == 1U,
        "changing a traced lexical guard did not rebuild and trace its selected host branch"
    );
    const auto stable_host = HostSnapshot::from_json(
        "expression-host-stable-leaf",
        3U,
        strata::data::parse_json(
            R"({"app":{"limits":{"minimum":100,"sibling":"still-cold"},"unrelated":"again"}})"
        )
    );
    static_cast<void>(host.adopt(stable_host));
    traced_scope.values.insert_or_assign("unrelated", Value("third"));
    const ExpressionValue exact_host_hit = runtime.evaluate_in(traced_filter, traced_scope);
    check(
        exact_host_hit.collection() != nullptr &&
            *exact_host_hit.collection() == *selected.collection() &&
            stable_host->evaluated_scalar_count() == 1U,
        "global host generation or an unread sibling invalidated the exact collection dependency"
    );

    traced_scope.contextual_host_roots.insert_or_assign(
        "app",
        Value(std::vector<std::pair<std::string, Value>>{
            {"limits", Value(std::vector<std::pair<std::string, Value>>{
                {"minimum", Value(100.0)},
                {"sibling", Value("context-cold")},
            })},
            {"unrelated", Value("context-first")},
        })
    );
    const ExpressionValue contextual = runtime.evaluate_in(traced_filter, traced_scope);
    traced_scope.contextual_host_roots.insert_or_assign(
        "app",
        Value(std::vector<std::pair<std::string, Value>>{
            {"limits", Value(std::vector<std::pair<std::string, Value>>{
                {"minimum", Value(100.0)},
                {"sibling", Value("context-changed")},
            })},
            {"unrelated", Value("context-second")},
        })
    );
    const ExpressionValue contextual_hit = runtime.evaluate_in(traced_filter, traced_scope);
    check(
        contextual.collection() != nullptr && contextual_hit.collection() != nullptr &&
            *contextual.collection() != *selected.collection() &&
            *contextual.collection() == *contextual_hit.collection(),
        "contextual origin or an unrelated contextual sibling was not tracked exactly"
    );
    traced_scope.contextual_host_roots.clear();

    HostStore nullable_host;
    ExpressionRuntime nullable_runtime(
        nullable_host,
        bundle->action_registry(),
        traced_scope
    );
    const auto explicit_null_host = HostSnapshot::from_json(
        "expression-host-nullability",
        1U,
        strata::data::parse_json(R"({"app":{"limits":{"minimum":null,"sibling":"cold"}}})")
    );
    static_cast<void>(nullable_host.adopt(explicit_null_host));
    const ExpressionValue explicit_null_view = nullable_runtime.evaluate_in(
        traced_filter,
        traced_scope
    );
    const auto missing_host = HostSnapshot::from_json(
        "expression-host-nullability",
        2U,
        strata::data::parse_json(R"({"app":{"limits":{"sibling":"cold"}}})")
    );
    static_cast<void>(nullable_host.adopt(missing_host));
    const ExpressionValue missing_view = nullable_runtime.evaluate_in(
        traced_filter,
        traced_scope
    );
    check(
        explicit_null_view.collection() != nullptr && missing_view.collection() != nullptr &&
            *explicit_null_view.collection() != *missing_view.collection() &&
            explicit_null_host->evaluated_scalar_count() == 0U &&
            missing_host->evaluated_scalar_count() == 0U,
        "collection cache collapsed an explicit-null host leaf into absence or evaluated a sibling"
    );
    nullable_runtime.clear_diagnostics();

    const auto action_expression = strata::data::parse_json(R"({
      "kind":"action","id":"state.set","path":"/action","arguments":[
        {"name":"name","value":{"kind":"literal","path":"/action/name","value":{"kind":"string","value":"count"}}},
        {"name":"value","value":{"kind":"literal","path":"/action/value","value":{"kind":"number","value":7}}}
      ]
    })");
    const ExpressionValue action = runtime.evaluate(action_expression);
    check(
        action.action() != nullptr && (*action.action())->action != nullptr &&
            (*action.action())->action->id() == "state.set" &&
            *(*action.action())->action->payload.field("name")->string() == "count",
        "portable IR typed action evaluation changed"
    );
    check(runtime.diagnostics().empty(), "valid portable IR expressions produced runtime diagnostics");
}

void test_target10_compile_diagnostics(const std::filesystem::path& registry_path) {
    using namespace strata;
    const data::JsonValue registry_document = data::parse_json(
        resource::load_utf8_resource(
            registry_path.parent_path(),
            resource::ResourceId::parse(registry_path.filename().string())
        )
    );
    compiler::SchemaRegistry schema = compiler::SchemaRegistry::parse(registry_document);
    const data::JsonValue scenario_document = data::parse_json(R"({
      "widgets":{"registry":"test","required":[],"definitions":[]},
      "actions":{"registry":"test","required":[],"definitions":[]},
      "host":[{
        "path":"request","nullable":false,
        "type":{"kind":"async","value":{"kind":"list","maximumItems":8,
          "elementNullable":false,"element":{"kind":"string"}}}
      }]
    })");
    schema.apply_scenario_declarations(scenario_document);
    const std::string invalid_source = R"(
      overlay Target {
        root Scroll(persistenceKey: "same.key") {
          state saved = persisted(format("{0}", "dynamic"), false);
          state flag = false;
          Button(
            label: "Toggle",
            onClick: action("state.toggle", name: "flag", undoLabel: format("{0}", "Toggle"))
          )
          Scroll(persistenceKey: "same.key")
          when request.status {
            "IDLE" -> { Text(text: "idle") }
          }
          Text(text: format("{0}", request.value))
          Text(text: persisted("orphan.value", "ignored"))
        }
      }
    )";
    const compiler::ParseResult invalid_parsed = compiler::parse_source(
        "target10-invalid.strata", invalid_source
    );
    check(invalid_parsed.diagnostics.empty(), "Target 10 invalid semantic fixture did not parse");
    const compiler::SemanticResult invalid = compiler::validate_semantics(
        invalid_parsed.file, schema
    );
    const auto has = [&invalid](const std::string_view code) {
        return std::ranges::any_of(invalid.diagnostics, [code](const compiler::Diagnostic& value) {
            return value.code == code;
        });
    };
    check(
        has("STRATA.DSL.SEMANTIC_ASYNC_WHEN_NOT_EXHAUSTIVE") &&
            has("STRATA.DSL.SEMANTIC_ASYNC_VALUE_ACCESS") &&
            has("STRATA.DSL.SEMANTIC_PERSISTENCE_KEY_STATIC") &&
            has("STRATA.DSL.SEMANTIC_DUPLICATE_PERSISTENCE_KEY") &&
            has("STRATA.DSL.SEMANTIC_UNDO_LABEL_STATIC") &&
            has("STRATA.DSL.SEMANTIC_PERSISTED_CONTEXT"),
        "Target 10 static authoring diagnostics are incomplete"
    );
    check(
        std::ranges::all_of(invalid.diagnostics, [&invalid_source](const compiler::Diagnostic& value) {
            return value.range.has_value() &&
                value.range->start.offset <= value.range->end.offset &&
                value.range->end.offset <= invalid_source.size();
        }),
        "Target 10 semantic diagnostic lost its source range"
    );

    const std::string flow_invalid_source = R"(
      overlay Target {
        root Panel {
          when request.status {
            "IDLE" -> { Text(text: "idle") }
            "LOADING" -> { Text(text: "loading") }
            "READY" -> { Text(text: format("{0}", count(request.value))) }
            "READY" -> { Text(text: "duplicate") }
            "FAILED" -> { Text(text: request.error.message) }
          }
          Text(text: format("{0}", request["value"]))
        }
      }
    )";
    const compiler::ParseResult flow_invalid_parsed = compiler::parse_source(
        "target10-flow-invalid.strata", flow_invalid_source
    );
    check(flow_invalid_parsed.diagnostics.empty(), "async flow invalid fixture did not parse");
    const compiler::SemanticResult flow_invalid = compiler::validate_semantics(
        flow_invalid_parsed.file, schema
    );
    check(
        std::ranges::any_of(flow_invalid.diagnostics, [](const compiler::Diagnostic& value) {
            return value.code == "STRATA.DSL.SEMANTIC_ASYNC_WHEN_NOT_EXHAUSTIVE";
        }) &&
        std::ranges::any_of(flow_invalid.diagnostics, [](const compiler::Diagnostic& value) {
            return value.code == "STRATA.DSL.SEMANTIC_ASYNC_VALUE_ACCESS";
        }),
        "duplicate async branches or indexed READY-value access bypassed flow diagnostics"
    );

    const std::string valid_source = R"(
      overlay Target {
        root Scroll(persistenceKey: "target.scroll") {
          state saved: boolean = persisted("target.saved", false);
          state savedList: List<string> = persisted("target.list", ["initial"]);
          when request.status {
            "IDLE" -> { Text(text: "idle") }
            "LOADING" -> { Text(text: "loading") }
            "READY" -> { Text(text: format("{0}", count(request.value))) }
            "FAILED" -> { Text(text: request.error.message) }
          }
          Button(label: "Toggle", onClick: action("state.toggle", name: "saved", undoLabel: "Toggle saved"))
          VirtualList(items: request, itemExtent: 24, loadingText: "loading", errorText: "error", emptyText: "empty")
        }
      }
    )";
    const compiler::ParseResult valid_parsed = compiler::parse_source(
        "target10-valid.strata", valid_source
    );
    check(valid_parsed.diagnostics.empty(), "Target 10 valid fixture did not parse");
    const compiler::SemanticResult valid = compiler::validate_semantics(valid_parsed.file, schema);
    check(valid.diagnostics.empty(), "valid Target 10 declarations produced semantic diagnostics");

    const auto bundle = runtime::ApplicationBundle::create(registry_document, &scenario_document);
    runtime::ApplicationContext application("target10-schema", bundle);
    application.configure_durable_store(runtime::DurableStoreAdapter{
        [](const std::string_view) -> std::optional<std::string> {
            return R"({"application":{"target.list":[1]},"commands":{},"format":"strata.durable","shell":{},"version":1,"widgets":{}})";
        },
        [](const std::string_view, const std::string_view) {},
    });
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"target10-valid.strata", valid_source},
        [](const std::string_view, const std::string_view) -> compiler::ModuleSource {
            throw compiler::ModuleLoadError("unexpected import");
        },
        0U
    );
    check(activation.activated(), "Target 10 persistence schema fixture did not activate");
    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult built = builder.build(runtime::LayerRole::overlay, "Target");
    check(built.root != nullptr, "Target 10 persistence schema fixture did not describe");
    check(
        application.durability().application_value("target.list") == nullptr,
        "schema-invalid persisted list elements entered application state"
    );
}

void test_durability_round_trip_migration_and_corruption() {
    using namespace strata::runtime;
    std::optional<std::string> bytes;
    DurableStoreAdapter adapter{
        [&bytes](const std::string_view) { return bytes; },
        [&bytes](const std::string_view, const std::string_view payload) {
            bytes = std::string(payload);
        },
    };
    DurableState first;
    check(first.configure("test.application", adapter).empty(), "empty durable load failed");
    first.set_application_value("settings.name", Value("Strata"));
    first.set_widget_value("view.tree", "strata.tree.expanded", Value(std::vector<Value>{Value("root")}));
    first.set_shell_value("window", Value(std::vector<std::pair<std::string, Value>>{{"x", Value(20.0)}}));
    first.set_command_recency("surface", {"save", "open"});
    check(!first.flush().has_value() && bytes.has_value(), "durable write-behind did not flush");

    DurableState restored;
    check(restored.configure("test.application", adapter).empty(), "durable round trip was rejected");
    check(
        restored.application_value("settings.name") != nullptr &&
            *restored.application_value("settings.name")->string() == "Strata",
        "application durable value did not round trip"
    );
    check(
        restored.widget_value("view.tree", "strata.tree.expanded") != nullptr &&
            restored.widget_value("view.tree", "strata.tree.expanded")->list() != nullptr,
        "widget durable value did not round trip"
    );
    check(
        restored.command_recency("surface") == std::vector<std::string>{"save", "open"},
        "command recency did not round trip"
    );

    bytes = R"({"application":{"legacy":true},"widgets":{}})";
    DurableState migrated;
    check(migrated.configure("test.application", adapter).empty(), "v0 durable migration failed");
    check(migrated.dirty(), "migrated durable document was not scheduled for canonical rewrite");
    check(!migrated.flush().has_value(), "migrated durable document rewrite failed");
    const strata::data::JsonValue migrated_json = strata::data::parse_json(*bytes);
    check(
        migrated_json.find("version") != nullptr &&
            migrated_json.find("version")->integer() != nullptr &&
            *migrated_json.find("version")->integer() == 1,
        "migrated durable document was not rewritten as v1"
    );

    bytes = "{broken";
    DurableState corrupt;
    const std::vector<DurableLoadIssue> corrupt_issues = corrupt.configure("test.application", adapter);
    check(
        corrupt_issues.size() == 1U &&
            corrupt_issues.front().code == "STRATA.DURABILITY.CORRUPT_PAYLOAD" &&
            corrupt.application_value("legacy") == nullptr,
        "corrupt durable data was not discarded diagnostically"
    );
    bytes = R"({"version":99,"application":{"bad":true}})";
    DurableState future;
    const std::vector<DurableLoadIssue> future_issues = future.configure("test.application", adapter);
    check(
        future_issues.size() == 1U &&
            future_issues.front().code == "STRATA.DURABILITY.UNKNOWN_VERSION" &&
            future.application_value("bad") == nullptr,
        "unknown durable version was partially applied"
    );
    bytes = R"({"format":"strata.durable","version":1,"application":{"valid":true},"widgets":[],"commands":{},"shell":{}})";
    DurableState malformed;
    const std::vector<DurableLoadIssue> malformed_issues = malformed.configure(
        "test.application", adapter
    );
    check(
        malformed_issues.size() == 1U &&
            malformed_issues.front().code == "STRATA.DURABILITY.MIGRATION_FAILED" &&
            malformed.application_value("valid") == nullptr,
        "wrong-shaped durable namespaces were partially applied"
    );
}

void test_declared_widget_persistence_is_exact() {
    using namespace strata;
    ui::WidgetRegistry widgets;
    const ui::WidgetLifecycle* table = widgets.find("Table");
    const runtime::Value table_widths(std::vector<runtime::Value>{runtime::Value(
        std::vector<std::pair<std::string, runtime::Value>>{
            {"id", runtime::Value("name")}, {"width", runtime::Value(240.0)},
        }
    )});
    check(
        table != nullptr && table->persistence.accepts != nullptr &&
            table->persistence.accepts("strata.table.columnWidths", table_widths),
        "Table rejected its own encoded durable column widths"
    );
    runtime::DurableState durable;
    static_cast<void>(durable.configure("widgets", {}));
    durable.set_widget_value("tree.view", "strata.tree.expanded", runtime::Value(
        std::vector<runtime::Value>{runtime::Value("root")}
    ));
    durable.set_widget_value("tree.view", "undeclared.internal", runtime::Value(true));

    ui::RetainedTree tree;
    tree.configure_persistence(
        [](const std::string_view type) {
            return type == "TreeView"
                ? std::vector<std::string>{"strata.tree.expanded"}
                : std::vector<std::string>{};
        },
        [&durable](
            const std::string_view,
            const std::string_view key,
            const std::string_view field
        ) -> std::optional<runtime::Value> {
            const runtime::Value* value = durable.widget_value(key, field);
            return value != nullptr ? std::optional<runtime::Value>(*value) : std::nullopt;
        },
        [&durable](
            const std::string_view key,
            const std::string_view field,
            const runtime::Value& value
        ) {
            durable.set_widget_value(std::string(key), std::string(field), value);
        }
    );
    ui::DescriptionNode::Properties properties;
    properties.emplace("persistenceKey", runtime::ExpressionValue(runtime::Value("tree.view")));
    static_cast<void>(tree.reconcile(ui::DescriptionNode::create(
        "TreeView", "tree", "test", "scope", std::move(properties)
    )));
    check(
        tree.root()->retained_value("strata.tree.expanded") != nullptr &&
            tree.root()->retained_value("undeclared.internal") == nullptr,
        "retained tree restored state outside the widget persistence contract"
    );
    static_cast<void>(tree.set_retained_value(
        tree.root()->identity(), "strata.tree.expanded",
        runtime::Value(std::vector<runtime::Value>{runtime::Value("next")})
    ));
    check(
        durable.widget_value("tree.view", "strata.tree.expanded") != nullptr &&
            *durable.widget_value("tree.view", "strata.tree.expanded")->list()->values.front().string() == "next",
        "declared retained mutation was not written behind"
    );
}

void test_async_debounce_latest_wins_and_owner_cancellation() {
    using namespace strata::runtime;
    const ValueSchemaPtr tree_item = ValueSchema::object({
        {"key", ValueSchema::scalar(ValueSchemaKind::key), true, false},
        {"parentKey", ValueSchema::union_of({
            ValueSchema::scalar(ValueSchemaKind::key),
            ValueSchema::scalar(ValueSchemaKind::null_value),
        }), true, false},
    });
    const ValueSchemaPtr tree_items = ValueSchema::list(tree_item, false, 8U);
    const std::optional<Value> normalized_tree = tree_items->normalize(Value(
        std::vector<Value>{Value(std::vector<std::pair<std::string, Value>>{
            {"key", Value("folder.child")}, {"parentKey", Value("folder")},
        })}
    ));
    check(
        normalized_tree.has_value() && normalized_tree->list() != nullptr &&
            normalized_tree->list()->values.front().field("key")->key() != nullptr &&
            normalized_tree->list()->values.front().field("parentKey")->key() != nullptr,
        "typed async JSON did not normalize nested key fields"
    );

    std::vector<AsyncRequest> begun;
    std::vector<std::uint64_t> cancelled;
    bool cancellation_reentry_accepted = false;
    Value published;
    AsyncDataService service(
        [](const std::string_view binding) { return binding == "search"; },
        [](const std::string_view, const Value& value) {
            return std::optional<Value>(value);
        },
        [&published](const Value& roots) { published = roots; }
    );
    service.set_adapter(AsyncHostAdapter{
        [&begun](const AsyncRequest& request) { begun.push_back(request); },
        [&cancelled, &cancellation_reentry_accepted, &service](const std::uint64_t id) {
            cancelled.push_back(id);
            cancellation_reentry_accepted = service.succeed(id, Value("cancel-race"));
        },
    });
    service.initialize({"search"});
    const std::uint64_t first = *service.query("search", Value("a"), "surface:field", 0, 100);
    const std::uint64_t second = *service.query("search", Value("ab"), "surface:field", 50, 100);
    static_cast<void>(first);
    service.advance(149);
    check(begun.empty(), "debounced async request began before its due time");
    service.advance(150);
    check(begun.size() == 1U && begun.front().id == second, "rapid async queries did not collapse");

    const std::uint64_t third = *service.query("search", Value("abc"), "surface:field", 160, 0);
    check(cancelled == std::vector<std::uint64_t>{second}, "re-query did not cancel begun host work");
    check(!cancellation_reentry_accepted, "cancel callback completed a request after invalidation");
    check(!service.succeed(second, Value("stale")), "stale async response defeated latest-wins");
    check(service.progress(third, AsyncProgress{1.0, 2.0, "half"}), "async progress was rejected");
    check(service.succeed(third, Value(std::vector<Value>{Value("ready")})), "async success was rejected");
    const Value ready = service.state("search");
    check(
        ready.field("status") != nullptr && *ready.field("status")->string() == "READY" &&
            ready.field("value") != nullptr && ready.field("value")->list() != nullptr,
        "async ready snapshot has the wrong typed shape"
    );

    const std::uint64_t fourth = *service.query("search", Value("again"), "surface:owner", 200, 0);
    static_cast<void>(fourth);
    check(service.cancel_owner("surface:owner") == 1U, "async owner cancellation did not cancel work");
    check(cancelled.size() == 2U && service.active_count() == 0U,
          "cancelled async owner retained active work");
    check(published.field("search") != nullptr, "async publisher omitted its declared root");

    static_cast<void>(service.query(
        "search", Value("children"), "surface:item:folder", 250, 0
    ));
    check(
        service.retain_owners("surface:", {"surface:node:tree"}) == 1U,
        "detached async item owner did not cancel its host work"
    );

    const std::uint64_t worker_request = *service.query(
        "search", Value("worker"), "surface:node:query", 300, 0
    );
    std::jthread completion([&service, worker_request] {
        check(
            service.post_progress(worker_request, AsyncProgress{1.0, 2.0, "worker"}),
            "worker-thread progress was not accepted into the owner mailbox"
        );
        check(
            service.post_succeed(worker_request, Value(std::vector<Value>{Value("thread-ready")})),
            "worker-thread success was not accepted into the owner mailbox"
        );
    });
    completion.join();
    service.advance(301);
    check(
        service.state("search").field("status") != nullptr &&
            *service.state("search").field("status")->string() == "READY",
        "owner-thread advance did not adopt worker-thread async completion"
    );
}

void test_application_undo_grouping_coalescing_and_invalidation() {
    using namespace strata::runtime;
    StateStore state;
    const StateAddress address{"scope", "count"};
    const StateSlot slot = StateSlot::from_initial("count", Value(0.0), "scope");
    static_cast<void>(state.read(address, slot));
    std::vector<std::string> invalidated;
    UndoManager undo([&invalidated](const std::string_view, const std::string_view action) {
        invalidated.emplace_back(action);
    });

    StateSnapshot before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(1.0)));
    undo.record("surface", before, state.snapshot(), UndoRecordOptions{"Adjust count", "slider", 10});
    before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(2.0)));
    undo.record("surface", before, state.snapshot(), UndoRecordOptions{"Adjust count", "slider", 20});
    check(undo.undo("surface", state), "coalesced undo was unavailable");
    check(state.find(address) != nullptr && *state.find(address)->number() == 0.0,
          "coalesced undo did not restore the first pre-change value");
    check(undo.redo("surface", state) && *state.find(address)->number() == 2.0,
          "coalesced redo did not restore the final value");

    undo.begin_group("surface", "Grouped edit");
    before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(3.0)));
    undo.record("surface", before, state.snapshot(), UndoRecordOptions{"first", std::nullopt, 30});
    before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(4.0)));
    undo.record("surface", before, state.snapshot(), UndoRecordOptions{"second", std::nullopt, 31});
    undo.end_group("surface");
    check(undo.undo("surface", state) && *state.find(address)->number() == 2.0,
          "undo group was not one atomic entry");

    before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(5.0)));
    undo.record("noop", before, state.snapshot(), UndoRecordOptions{"Drag", "return", 40});
    before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(2.0)));
    undo.record("noop", before, state.snapshot(), UndoRecordOptions{"Drag", "return", 41});
    check(
        !undo.status("noop").can_undo,
        "coalescing an edit back to its initial value left a stuck undo entry"
    );
    undo.begin_group("noop-group", "Round trip");
    before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(6.0)));
    undo.record("noop-group", before, state.snapshot(), UndoRecordOptions{"up", std::nullopt, 50});
    before = state.snapshot();
    static_cast<void>(state.write(address, slot, Value(2.0)));
    undo.record("noop-group", before, state.snapshot(), UndoRecordOptions{"down", std::nullopt, 51});
    undo.end_group("noop-group");
    check(
        !undo.status("noop-group").can_undo,
        "grouping an edit back to its initial value left a stuck undo entry"
    );

    undo.invalidate("surface", "state.external");
    check(!undo.status("surface").can_undo && !undo.status("surface").can_redo &&
              invalidated == std::vector<std::string>{"state.external"},
          "opted-out mutation did not clear and diagnose the undo stack");
}

void test_layer_stack_and_declarative_registry() {
    using namespace strata::runtime;
    std::uint64_t invalidations = 0U;
    LayerStack stack([&invalidations] { ++invalidations; });
    DeclarativeLayerRegistry registry;
    registry.register_screen("Main", "screen:main");
    registry.register_screen("Settings", "screen:settings");
    registry.register_overlay("Inspector", "overlay:inspector");

    check(
        registry.execute(stack, DeclarativeLayerOperation::push, "Main").status ==
            LayerOperationStatus::handled,
        "declarative screen push failed"
    );
    check(
        registry.execute(stack, DeclarativeLayerOperation::push, "Main").status ==
            LayerOperationStatus::ignored,
        "duplicate declarative screen push was not ignored"
    );
    check(
        registry.execute(stack, DeclarativeLayerOperation::show, "Inspector").status ==
            LayerOperationStatus::handled,
        "declarative overlay show failed"
    );
    check(
        registry.execute(stack, DeclarativeLayerOperation::show, "Inspector").status ==
            LayerOperationStatus::handled,
        "registered overlay re-show was not handled"
    );
    check(
        registry.execute(stack, DeclarativeLayerOperation::replace, "Settings").status ==
            LayerOperationStatus::handled && stack.active_screen() == "screen:settings",
        "declarative screen replacement failed"
    );
    check(
        stack.snapshot() == std::vector<LayerSnapshot>{
            {"screen:settings", LayerRole::screen},
            {"overlay:inspector", LayerRole::overlay},
        },
        "layer snapshot ordering changed"
    );
    check(
        registry.execute(stack, DeclarativeLayerOperation::hide, "Inspector").status ==
            LayerOperationStatus::handled,
        "declarative overlay hide failed"
    );
    check(
        registry.execute(stack, DeclarativeLayerOperation::pop).status == LayerOperationStatus::handled &&
            registry.execute(stack, DeclarativeLayerOperation::pop).status == LayerOperationStatus::ignored,
        "declarative screen pop behavior changed"
    );
    check(invalidations == stack.generation(), "layer invalidation and generation diverged");
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        const auto run = [](const std::string_view name, auto&& operation) {
            try {
                operation();
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(name) + ": " + error.what());
            }
        };
        test_immutable_values_and_runtime_schemas();
        test_host_snapshots_are_lazy_and_generation_invalidated();
        test_state_store_and_typed_mutations();
        test_durability_round_trip_migration_and_corruption();
        test_declared_widget_persistence_is_exact();
        test_async_debounce_latest_wins_and_owner_cancellation();
        test_application_undo_grouping_coalescing_and_invalidation();
        test_action_contracts_dispatch_and_handler_lifetime();
        test_layer_stack_and_declarative_registry();
        test_canonical_runtime_diagnostic_store();
        if (argument_count >= 2) {
            run("neutral registry", [&] {
                test_neutral_registry_projects_runtime_action_contracts(arguments[1]);
            });
            run("animation validation", [&] {
                test_animation_validation_is_total(arguments[1]);
            });
            run("Target 10 compile diagnostics", [&] {
                test_target10_compile_diagnostics(arguments[1]);
            });
            run("application isolation", [&] {
                test_application_bundle_and_runtime_isolation(arguments[1]);
            });
            run("portable expression runtime", [&] {
                test_portable_ir_expression_runtime(arguments[1]);
            });
        }
        if (argument_count >= 3 && std::string_view(arguments[2]).size() != 0U) {
            run("transactional activation", [&] {
                test_transactional_activation_and_last_good_reload(arguments[1], arguments[2]);
            });
        }
        std::cout << "strata_runtime_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_runtime_tests: " << error.what() << '\n';
        return 1;
    }
}
