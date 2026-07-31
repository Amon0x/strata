#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "compiler/source.hpp"
#include "data/json.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "ui/surface.hpp"
#include "ui/text.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::shared_ptr<const strata::runtime::ApplicationBundle> load_bundle(
    const std::filesystem::path& registry_path
) {
    const std::string registry = strata::resource::load_utf8_resource(
        registry_path.parent_path(),
        strata::resource::ResourceId::parse(registry_path.filename().generic_string())
    );
    return strata::runtime::ApplicationBundle::create(strata::data::parse_json(registry));
}

[[nodiscard]] strata::compiler::ModuleLoader no_imports() {
    return [](const std::string_view, const std::string_view path) -> strata::compiler::ModuleSource {
        throw strata::compiler::ModuleLoadError(
            "unexpected binding fixture import '" + std::string(path) + "'"
        );
    };
}

[[nodiscard]] const strata::runtime::ActivationDiagnostic* diagnostic(
    const strata::runtime::ActivationResult& result,
    const std::string_view code
) {
    for (const strata::runtime::ActivationDiagnostic& value : result.diagnostics) {
        if (value.code == code) return &value;
    }
    return nullptr;
}

[[nodiscard]] const std::string* string_property(
    const strata::ui::RetainedNode* node,
    const std::string_view name
) {
    if (node == nullptr) return nullptr;
    const auto property = node->description().properties.find(name);
    if (property == node->description().properties.end() || property->second.value() == nullptr) {
        return nullptr;
    }
    return property->second.value()->string();
}

void test_semantic_binding_rejections(
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    const auto compile = [&bundle](std::string source) {
        strata::runtime::ApplicationContext application("binding-rejection", bundle);
        return application.compile_and_activate(
            strata::compiler::ModuleSource{"binding-rejection.strata", std::move(source)},
            no_imports(),
            0U
        );
    };

    const strata::runtime::ActivationResult conflict = compile(R"(
component InvalidConflict() {
  state value = "seed";
  TextBox(key: "binding.conflict", bind: value, text: value)
}
overlay Main { root InvalidConflict() }
)");
    check(
        conflict.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(conflict, "STRATA.DSL.SEMANTIC_BINDING_CONFLICT") != nullptr,
        "binding shorthand accepted an explicit controlled-value conflict"
    );

    const strata::runtime::ActivationResult target = compile(R"(
component InvalidTarget() {
  state value = "seed";
  derived copy = value;
  TextBox(key: "binding.target", bind: copy)
}
overlay Main { root InvalidTarget() }
)");
    check(
        target.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(target, "STRATA.DSL.SEMANTIC_BINDING_TARGET") != nullptr,
        "binding shorthand accepted a derived value instead of declaration-owned state"
    );
}

void test_binding_initial_value_and_round_trip(
    const std::filesystem::path& registry_path,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component BoundToggle(enabled: boolean) {
  Toggle(
    key: "binding.toggle",
    label: "Enabled",
    checked: enabled,
    onChange: action("state.setFromEvent", name: "enabled", undoLabel: "Toggle enabled")
  )
}

component BoundControls() {
  state value = "Seed value";
  state enabled = true;
  Panel(key: "binding.root", layout: { kind: "COLUMN" }) {
    TextBox(
      key: "binding.editor", bind: value,
      undoLabel: "Edit value", undoCoalesce: "binding.value.edit"
    )
    BoundToggle(enabled: enabled)
    Text(key: "binding.value", text: value)
    Text(key: "binding.enabled", text: format("Enabled {0}", enabled))
  }
}
overlay Main { root BoundControls() }
)";

    strata::runtime::ApplicationContext application("binding-round-trip", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"binding-round-trip.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "valid binding shorthand fixture did not activate");

    strata::ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 640;
    environment.framebuffer_height = 480;
    environment.logical_width = 640.0;
    environment.logical_height = 480.0;
    environment.reduced_motion = true;
    environment.input = strata::ui::SurfaceInputCapabilities{
        true,
        strata::ui::PointerPrecision::fine,
        true,
        false,
        true,
        true,
        false,
    };
    const std::filesystem::path resources = registry_path.parent_path().parent_path();
    strata::ui::Surface surface(
        "binding-round-trip",
        application,
        strata::runtime::LayerRole::overlay,
        "Main",
        environment,
        strata::ui::TextEngine::load_control_font(
            resources,
            strata::resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        )
    );
    static_cast<void>(surface.frame(1'000'000));

    const strata::ui::RetainedNode* editor = surface.tree().find_key("binding.editor");
    check(editor != nullptr, "bound TextBox was not retained");
    const std::optional<strata::ui::TextEditorSnapshot> initial =
        surface.input().editor_snapshot(editor->identity());
    check(
        initial.has_value() && initial->text == "Seed value",
        "binding shorthand did not project retained state into the controlled TextBox value"
    );

    static_cast<void>(surface.input().click("binding.editor"));
    static_cast<void>(surface.input().key(
        "a",
        strata::ui::KeyModifiers{false, true, false, false}
    ));
    const strata::ui::InputOperationResult changed = surface.input().text("Updated value");
    check(
        !changed.action_outcomes.empty(),
        "bound TextBox mutation did not dispatch its synthesized state action"
    );
    static_cast<void>(surface.frame(2'000'000));
    const strata::ui::RetainedNode* value = surface.tree().find_key("binding.value");
    check(
        string_property(value, "text") != nullptr &&
            *string_property(value, "text") == "Updated value",
        "bound TextBox event did not round-trip into retained state and dependent UI"
    );
    static_cast<void>(surface.input().key(
        "z",
        strata::ui::KeyModifiers{false, true, false, false}
    ));
    static_cast<void>(surface.frame(2'100'000));
    value = surface.tree().find_key("binding.value");
    check(
        string_property(value, "text") != nullptr &&
            *string_property(value, "text") == "Seed value" &&
            !application.undo().status("binding-round-trip").can_undo,
        "focused editor local undo did not win or left a no-op application entry"
    );

    const strata::ui::InputOperationResult toggled = surface.input().click("binding.toggle");
    check(
        !toggled.action_outcomes.empty(),
        "bound Toggle mutation did not dispatch its synthesized state action"
    );
    static_cast<void>(surface.frame(3'000'000));
    const strata::ui::RetainedNode* enabled = surface.tree().find_key("binding.enabled");
    check(
        string_property(enabled, "text") != nullptr &&
            *string_property(enabled, "text") == "Enabled false",
        "bound Toggle event did not round-trip into retained state and dependent UI"
    );
    static_cast<void>(surface.input().click("binding.editor"));
    static_cast<void>(surface.input().key(
        "z",
        strata::ui::KeyModifiers{false, true, false, false}
    ));
    static_cast<void>(surface.frame(3'100'000));
    enabled = surface.tree().find_key("binding.enabled");
    check(
        string_property(enabled, "text") != nullptr &&
            *string_property(enabled, "text") == "Enabled false" &&
            application.undo().status("binding-round-trip").can_undo,
        "focused editor shortcut incorrectly consumed application undo state"
    );
    static_cast<void>(surface.dispatch_action(
        "focus.request",
        strata::runtime::Value(std::vector<std::pair<std::string, strata::runtime::Value>>{
            {"key", strata::runtime::Value(strata::runtime::KeyValue{"binding.toggle"})},
        }),
        "test",
        std::nullopt,
        strata::runtime::Value{}
    ));
    static_cast<void>(surface.frame(3'200'000));
    static_cast<void>(surface.input().key(
        "z",
        strata::ui::KeyModifiers{false, true, false, false}
    ));
    static_cast<void>(surface.frame(3'300'000));
    enabled = surface.tree().find_key("binding.enabled");
    check(
        string_property(enabled, "text") != nullptr &&
            *string_property(enabled, "text") == "Enabled true",
        "application undo did not resume after editor focus moved"
    );
}

void test_typed_host_keys_and_derived_collection_metadata(
    const std::filesystem::path& registry_path
) {
    const std::string registry = strata::resource::load_utf8_resource(
        registry_path.parent_path(),
        strata::resource::ResourceId::parse(registry_path.filename().generic_string())
    );
    const strata::data::JsonValue schemas = strata::data::parse_json(R"({
      "widgets":{"definitions":[]},
      "actions":{"definitions":[]},
      "host":[{
        "path":"data",
        "type":{
          "kind":"object",
          "label":"typed host fixture",
          "allowUnknownFields":false,
          "valueNullable":false,
          "fields":[{
            "name":"rows",
            "required":true,
            "nullable":false,
            "type":{
              "kind":"list",
              "elementNullable":false,
              "element":{
                "kind":"object",
                "label":"typed row",
                "allowUnknownFields":false,
                "valueNullable":false,
                "fields":[
                  {"name":"rowKey","type":{"kind":"key"},"required":true,"nullable":false},
                  {"name":"label","type":{"kind":"string"},"required":true,"nullable":false}
                ]
              }
            }
          }]
        }
      }]
    })");
    const auto bundle = strata::runtime::ApplicationBundle::create(
        strata::data::parse_json(registry),
        &schemas
    );
    strata::runtime::ApplicationContext application("typed-host-derived", bundle);
    static_cast<void>(application.host().adopt(bundle->host_snapshot(
        "typed-host",
        1U,
        strata::data::parse_json(R"({
          "data":{"rows":[
            {"rowKey":"row.alpha","label":"alpha"},
            {"rowKey":"row.beta","label":"beta"}
          ]}
        })")
    )));
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"typed-host-derived.strata", R"(
component TypedProjection() {
  derived matches = filter(data.rows, row -> contains(row.label, "a"));
  Panel() {
    Text(
      key: "typed.metadata",
      text: format("{0}:{1}:{2}:{3}:{4}", matches.matched, matches.rangeStart, matches.rangeEnd, matches.cacheHits, matches.rebuilds)
    )
    Button(
      key: "typed.reveal",
      label: "Reveal",
      onClick: action("reveal.request", key: data.rows[0].rowKey, scroll: "typed.scroll", focus: true, padding: 10)
    )
  }
}
overlay Main { root TypedProjection() }
)"},
        no_imports(),
        0U
    );
    check(activation.activated(), "typed host/derived fixture did not activate");

    strata::ui::DescriptionBuilder builder(application);
    const strata::ui::DescriptionBuildResult description = builder.build(
        strata::runtime::LayerRole::overlay,
        "Main"
    );
    check(description.diagnostics.empty(), "typed host/derived fixture produced runtime diagnostics");
    strata::ui::RetainedTree tree;
    static_cast<void>(tree.reconcile(description.root));
    const strata::ui::RetainedNode* metadata = tree.find_key("typed.metadata");
    check(
        string_property(metadata, "text") != nullptr &&
            *string_property(metadata, "text") == "2:0:2:0:1",
        "derived collection metadata was materialized into a plain list"
    );
    const strata::ui::RetainedNode* reveal = tree.find_key("typed.reveal");
    check(reveal != nullptr, "typed reveal action node was not retained");
    const auto action_property = reveal->description().properties.find("onClick");
    const strata::runtime::ExpressionValue* action =
        action_property != reveal->description().properties.end()
            ? &action_property->second
            : nullptr;
    const strata::runtime::Value* key = action != nullptr && action->action() != nullptr &&
            *action->action() != nullptr && (*action->action())->action != nullptr
        ? (*action->action())->action->payload.field("key")
        : nullptr;
    check(
        key != nullptr && key->key() != nullptr && key->key()->value == "row.alpha",
        "schema-declared host key remained an untyped JSON string"
    );
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2) throw std::invalid_argument("expected registry path");
        const std::filesystem::path registry_path(arguments[1]);
        const auto bundle = load_bundle(registry_path);
        test_semantic_binding_rejections(bundle);
        test_binding_initial_value_and_round_trip(registry_path, bundle);
        test_typed_host_keys_and_derived_collection_metadata(registry_path);
        std::cout << "strata_widget_binding_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_widget_binding_tests: " << error.what() << '\n';
        return 1;
    }
}
