#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "compiler/source.hpp"
#include "data/json.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "ui/frame_snapshot.hpp"
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

void test_section_content_and_activation_contract(
    const std::filesystem::path& registry_path,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component SectionFixture() {
  state name = "Guest";
  Section(key: "section.main", label: "Profile", defaultExpanded: true) {
    TextBox(key: "section.editor", bind: name)
    Button(key: "section.button", label: "Apply")
    Text(key: "section.copy", text: name)
  }
}
overlay Main { root SectionFixture() }
)";

    strata::runtime::ApplicationContext application("section-contract", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"section-contract.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "Section content contract fixture did not activate");

    strata::ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 520;
    environment.framebuffer_height = 320;
    environment.logical_width = 520.0;
    environment.logical_height = 320.0;
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
        "section-contract",
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

    const auto near = [](const double left, const double right) {
        return std::abs(left - right) <= 0.001;
    };
    const strata::ui::RetainedNode* section = surface.tree().find_key("section.main");
    const strata::ui::RetainedNode* editor = surface.tree().find_key("section.editor");
    const strata::ui::RetainedNode* button = surface.tree().find_key("section.button");
    const strata::ui::RetainedNode* copy = surface.tree().find_key("section.copy");
    check(
        section != nullptr && editor != nullptr && button != nullptr && copy != nullptr &&
            section->children().size() == 2U,
        "Section did not retain its header/content structure"
    );
    const strata::ui::LayoutRecord* section_layout = surface.layout().find(section->identity());
    const strata::ui::LayoutRecord* header_layout = surface.layout().find(
        section->children().front()->identity()
    );
    const strata::ui::LayoutRecord* editor_layout = surface.layout().find(editor->identity());
    const strata::ui::LayoutRecord* button_layout = surface.layout().find(button->identity());
    const strata::ui::LayoutRecord* copy_layout = surface.layout().find(copy->identity());
    check(
        section_layout != nullptr && header_layout != nullptr && editor_layout != nullptr &&
            button_layout != nullptr && copy_layout != nullptr,
        "Section content contract lost layout records"
    );
    check(
        near(header_layout->bounds.height, 36.0) &&
            near(editor_layout->bounds.x, section_layout->bounds.x + 10.0) &&
            near(editor_layout->bounds.width, section_layout->bounds.width - 20.0) &&
            near(button_layout->bounds.width, section_layout->bounds.width - 20.0) &&
            near(button_layout->bounds.y - editor_layout->bounds.bottom(), 8.0) &&
            near(copy_layout->bounds.y - button_layout->bounds.bottom(), 8.0) &&
            near(section_layout->bounds.bottom() - copy_layout->bounds.bottom(), 10.0),
        "Section defaults did not provide a distinct padded, stretched content region"
    );
    const strata::data::JsonValue* section_semantics = surface.semantics().find(
        section->identity()
    );
    const strata::data::JsonValue* semantic_children = section_semantics != nullptr
        ? section_semantics->find("children")
        : nullptr;
    check(
        semantic_children != nullptr && semantic_children->array() != nullptr &&
            semantic_children->array()->size() == 3U,
        "Section internal layout wrappers leaked into its semantic children"
    );

    static_cast<void>(surface.input().click("section.editor"));
    static_cast<void>(surface.frame(2'000'000));
    section = surface.tree().find_key("section.main");
    editor = surface.tree().find_key("section.editor");
    const strata::runtime::Value* expanded = section != nullptr
        ? section->retained_value("$expanded")
        : nullptr;
    check(
        section != nullptr && editor != nullptr && surface.input().focused(editor->identity()) &&
            (expanded == nullptr || (expanded->boolean() != nullptr && *expanded->boolean())),
        "clicking a Section child collapsed the parent instead of focusing the child"
    );

    static_cast<void>(surface.input().key(
        "a",
        strata::ui::KeyModifiers{false, true, false, false}
    ));
    static_cast<void>(surface.input().text("Alice"));
    static_cast<void>(surface.frame(3'000'000));
    check(
        string_property(surface.tree().find_key("section.copy"), "text") != nullptr &&
            *string_property(surface.tree().find_key("section.copy"), "text") == "Alice",
        "Section child editor did not remain usable after pointer focus"
    );

    static_cast<void>(surface.input().click("section.main"));
    static_cast<void>(surface.frame(4'000'000));
    section = surface.tree().find_key("section.main");
    expanded = section != nullptr ? section->retained_value("$expanded") : nullptr;
    check(
        expanded != nullptr && expanded->boolean() != nullptr && !*expanded->boolean(),
        "Section header did not collapse its content"
    );
    static_cast<void>(surface.input().click("section.main"));
    static_cast<void>(surface.frame(5'000'000));
    section = surface.tree().find_key("section.main");
    expanded = section != nullptr ? section->retained_value("$expanded") : nullptr;
    check(
        expanded != nullptr && expanded->boolean() != nullptr && *expanded->boolean(),
        "collapsed Section header could not be reopened"
    );
}

void test_range_and_choice_control_defaults(
    const std::filesystem::path& registry_path,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component ControlDefaults() {
  state quality = "balanced";
  state mode = "windowed";
  Panel(layout: { kind: "COLUMN", width: 420, height: "content", gap: 8 }) {
    Select(
      key: "defaults.select",
      options: [
        { id: "fast", label: "Fast" },
        { id: "balanced", label: "Balanced" },
        { id: "quality", label: "High quality" }
      ],
      bind: quality
    )
    RadioGroup(
      key: "defaults.radio",
      options: [
        { id: "windowed", label: "Windowed" },
        { id: "borderless", label: "Borderless" },
        { id: "fullscreen", label: "Fullscreen" }
      ],
      bind: mode
    )
  }
}
overlay Main { root ControlDefaults() }
)";

    strata::runtime::ApplicationContext application("control-defaults", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"control-defaults.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "range/choice defaults fixture did not activate");

    strata::ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 520;
    environment.framebuffer_height = 320;
    environment.logical_width = 520.0;
    environment.logical_height = 320.0;
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
        "control-defaults",
        application,
        strata::runtime::LayerRole::overlay,
        "Main",
        environment,
        strata::ui::TextEngine::load_control_font(
            resources,
            strata::resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        )
    );
    const strata::ui::SurfaceFrame frame = surface.frame(1'000'000);

    const strata::ui::RetainedNode* select = surface.tree().find_key("defaults.select");
    const strata::ui::RetainedNode* radio = surface.tree().find_key("defaults.radio");
    const strata::ui::LayoutRecord* select_layout = select != nullptr
        ? surface.layout().find(select->identity())
        : nullptr;
    const strata::ui::LayoutRecord* radio_layout = radio != nullptr
        ? surface.layout().find(radio->identity())
        : nullptr;
    check(
        select_layout != nullptr && select_layout->bounds.height == 32.0 &&
            radio_layout != nullptr && radio != nullptr && radio->children().size() == 3U,
        "range/choice defaults lost their standard control geometry"
    );
    for (const auto& child : radio->children()) {
        const strata::ui::LayoutRecord* row = surface.layout().find(child->identity());
        const auto style = child->description().properties.find("$layout");
        const strata::runtime::Value* layout = style != child->description().properties.end()
            ? style->second.value()
            : nullptr;
        check(
            row != nullptr && row->bounds.width == radio_layout->bounds.width &&
                layout != nullptr && layout->field("background") != nullptr &&
                layout->field("background")->kind() == strata::runtime::ValueKind::null_value &&
                layout->field("border") != nullptr &&
                layout->field("border")->kind() == strata::runtime::ValueKind::null_value,
            "RadioGroup option rows did not remain full-width transparent internals"
        );
    }
    const strata::data::JsonValue* radio_semantics = surface.semantics().find(radio->identity());
    const strata::data::JsonValue* semantic_children = radio_semantics != nullptr
        ? radio_semantics->find("children")
        : nullptr;
    check(
        semantic_children != nullptr && semantic_children->array() != nullptr &&
            semantic_children->array()->size() == 3U,
        "RadioGroup synthesized rows duplicated its virtual radio semantics"
    );

    const strata::data::JsonValue snapshot = strata::ui::surface_frame_snapshot(surface, frame);
    const strata::data::JsonValue* commands = snapshot.find("renderCommands");
    bool found_chevron = false;
    bool found_image = false;
    if (commands != nullptr && commands->array() != nullptr) {
        for (const strata::data::JsonValue& command : *commands->array()) {
            const strata::data::JsonValue* kind = command.find("kind");
            if (kind == nullptr || kind->string() == nullptr) continue;
            found_chevron = found_chevron || *kind->string() == "path";
            found_image = found_image || *kind->string() == "image";
        }
    }
    check(
        found_chevron && !found_image,
        "Select default indicator still depended on a host-provided image"
    );
}

void test_scrolled_clipped_subtree_render_cache(
    const std::filesystem::path& registry_path,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
overlay Main {
  root Scroll(
    key: "cache.scroll",
    vertical: true,
    horizontal: false,
    layout: { kind: "SCROLL", width: 280, height: 120 }
  ) {
    Panel(key: "cache.top", layout: { width: 240, height: 160, clip: true }) {
      Text(text: "Top clipped content")
    }
    Panel(key: "cache.bottom", layout: { width: 240, height: 160, clip: true }) {
      Text(text: "Bottom clipped content")
    }
  }
}
)";

    strata::runtime::ApplicationContext application("scroll-clip-cache", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"scroll-clip-cache.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "scroll clip-cache fixture did not activate");

    strata::ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 320;
    environment.framebuffer_height = 180;
    environment.logical_width = 320.0;
    environment.logical_height = 180.0;
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
        "scroll-clip-cache",
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
    check(
        surface.animate_scroll_to(strata::ui::ScrollAnimationRequest{
            "cache.scroll", std::nullopt, 160.0, "standard", std::nullopt,
        }),
        "scroll clip-cache fixture did not accept a scroll offset"
    );
    static_cast<void>(surface.frame(2'000'000));

    const strata::ui::RetainedNode* bottom = surface.tree().find_key("cache.bottom");
    const strata::ui::LayoutRecord* bottom_layout = bottom != nullptr
        ? surface.layout().find(bottom->identity())
        : nullptr;
    check(
        bottom_layout != nullptr && bottom_layout->clip.has_value() &&
            !bottom_layout->clip->empty(),
        "scrolled clipped subtree did not enter the viewport"
    );
    bool emitted_current_clip = false;
    for (const strata::ui::RenderCommand& command : surface.render_commands().commands()) {
        const auto* clip = std::get_if<strata::ui::ClipPushRenderCommand>(&command);
        if (clip != nullptr && clip->rect == *bottom_layout->clip) {
            emitted_current_clip = true;
            break;
        }
    }
    check(
        emitted_current_clip,
        "retained subtree translation reused a stale pre-scroll clip"
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
        test_section_content_and_activation_contract(registry_path, bundle);
        test_range_and_choice_control_defaults(registry_path, bundle);
        test_scrolled_clipped_subtree_render_cache(registry_path, bundle);
        test_typed_host_keys_and_derived_collection_metadata(registry_path);
        std::cout << "strata_widget_binding_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_widget_binding_tests: " << error.what() << '\n';
        return 1;
    }
}
