#include <algorithm>
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
#include "ui/presentation_geometry.hpp"
#include "ui/surface.hpp"
#include "ui/text.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::shared_ptr<const strata::runtime::ApplicationBundle> load_bundle() {
    return strata::runtime::ApplicationBundle::create();
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

    const strata::runtime::ActivationResult layout = compile(R"(
overlay Main {
  root Panel(
    layout: {
      kind: "GRID",
      width: 200,
      height: 100,
      justifyContent: "CENTER"
    }
  )
}
)");
    check(
        layout.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(layout, "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND") != nullptr,
        "kind-specific layout validation accepted a property the selected engine ignores"
    );

    const strata::runtime::ActivationResult inherited_layout = compile(R"(
style GridAlignment { justifyContent: "CENTER"; }
style InvalidGrid extends GridAlignment { kind: "GRID"; }
overlay Main { root Panel(style: InvalidGrid) }
)");
    check(
        inherited_layout.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                inherited_layout,
                "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND"
            ) != nullptr,
        "kind-specific layout validation ignored composed style properties"
    );

    const strata::runtime::ActivationResult split_layout = compile(R"(
style GridAlignment { justifyContent: "CENTER"; }
overlay Main {
  root Panel(style: GridAlignment, layout: { kind: "GRID" })
}
)");
    check(
        split_layout.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                split_layout,
                "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND"
            ) != nullptr,
        "kind-specific layout validation did not combine style and layout arguments"
    );

    const strata::runtime::ActivationResult implicit_layout = compile(R"(
overlay Main {
  root Panel(layout: { gap: 8 })
}
)");
    check(
        implicit_layout.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                implicit_layout,
                "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND"
            ) != nullptr,
        "kind-specific layout validation ignored Panel's implicit layout kind"
    );

    const strata::runtime::ActivationResult defaulted_layout = compile(R"(
style GridAlignment { justifyContent: "CENTER"; }
component DefaultedGrid() {
  defaults: { Panel: { style: GridAlignment } }
  Panel {
    Slot(name: "content")
  }
}
overlay Main {
  root DefaultedGrid() {
    Panel(layout: { kind: "GRID" })
  }
}
)");
    check(
        defaulted_layout.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                defaulted_layout,
                "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND"
            ) != nullptr,
        "kind-specific layout validation ignored component widget-default styles"
    );

    const strata::runtime::ActivationResult dynamic_layout = compile(R"(
component DynamicLayer(justification: justify) {
  Panel(layout: { kind: "PANEL", justifyContent: justification })
}
overlay Main {
  root DynamicLayer(justification: "SPACE_BETWEEN")
}
)");
    check(
        dynamic_layout.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                dynamic_layout,
                "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND"
            ) != nullptr,
        "layered layout accepted a dynamic justification type containing SPACE_* values"
    );

    const strata::runtime::ActivationResult layered_layout = compile(R"(
component Layer(justification: layerJustify) {
  Panel(layout: { kind: "PANEL", justifyContent: justification })
}
overlay Main {
  root Layer(justification: "CENTER")
}
)");
    check(
        layered_layout.activated(),
        "layered layout rejected a constrained dynamic justification"
    );

    const strata::runtime::ActivationResult unknown_template = compile(R"(
overlay Main {
  root Button(
    key: "template.unknown",
    label: "Unknown",
    presentationTemplate: MissingPresentation
  )
}
)");
    check(
        unknown_template.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                unknown_template,
                "STRATA.DSL.SEMANTIC_UNKNOWN_COMPONENT_TEMPLATE"
            ) != nullptr,
        "unknown authored presentation template passed semantic validation"
    );

    const strata::runtime::ActivationResult wrong_template = compile(R"(
component WrongButtonPresentation(key: key, control: progressState) {
  Text(key: key, text: format("{0}", control.fraction))
}
overlay Main {
  root Button(
    key: "template.wrong",
    label: "Wrong",
    presentationTemplate: WrongButtonPresentation
  )
}
)");
    check(
        wrong_template.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                wrong_template,
                "STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_PARAMETER"
            ) != nullptr,
        "authored presentation template accepted an incompatible typed parameter"
    );

    const strata::runtime::ActivationResult bound_owner_parameter = compile(R"(
component BoundOwnerPresentation(key: key, label: string, control: buttonState) {
  Text(key: key, text: label)
}
overlay Main {
  root Button(
    key: "template.bound-owner",
    label: "Wrong",
    presentationTemplate: BoundOwnerPresentation(label: "Override")
  )
}
)");
    check(
        bound_owner_parameter.status ==
                strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                bound_owner_parameter,
                "STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_BOUND_OWNER_PARAMETER"
            ) != nullptr,
        "partial component template overrode a parameter owned by its widget"
    );

    const strata::runtime::ActivationResult unkeyed_template = compile(R"(
component UnkeyedButtonPresentation(key: key) {
  Text(key: key, text: "Unkeyed")
}
overlay Main {
  root Button(
    label: "Unkeyed",
    presentationTemplate: UnkeyedButtonPresentation
  )
}
)");
    check(
        unkeyed_template.status == strata::runtime::ActivationStatus::rejected_compile &&
            diagnostic(
                unkeyed_template,
                "STRATA.DSL.SEMANTIC_PRESENTATION_KEY_REQUIRED"
            ) != nullptr,
        "unkeyed authored presentation owner passed semantic validation"
    );

    const strata::runtime::ActivationResult forwarded_template = compile(R"(
component ForwardedButton(
  key: key,
  label: string,
  prefix: string,
  control: buttonState
) {
  Text(key: key, text: format("{0}{1}", prefix, label))
}
component ButtonOwner(template: buttonTemplate) {
  Button(
    key: "template.forwarded",
    label: "Forwarded",
    presentationTemplate: template
  )
}
overlay Main {
  root ButtonOwner(template: ForwardedButton(prefix: "Forwarded: "))
}
)");
    check(
        forwarded_template.activated(),
        "typed authored presentation template could not be forwarded through a component"
    );
}

void test_binding_initial_value_and_round_trip(
    const std::filesystem::path& resource_root,
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
    const std::filesystem::path& resources = resource_root;
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

void test_authored_control_presentations(
    const std::filesystem::path& resource_root,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
style AuthoredThumbOff {
  width: 12;
  height: 12;
  scale: 0.5;
  background: #FFFFFFFF;
  border: null;
}

style AuthoredThumbOn extends AuthoredThumbOff {
  scale: 1;
}

style StateStyleBase { radius: 1; }
style StateStyleMiddle { radius: 5; }
style StateStyleFinal { radius: 9; }

component AuthoredTogglePresentation(
  key: key,
  label: string,
  description: string,
  control: toggleState
) {
  Panel(
    key: key,
    layout: { kind: "ROW", width: 180, height: 28, alignItems: "CENTER" }
  ) {
    Text(
      key: "authored.toggle.copy",
      text: format(
        "{0}:{1}:{2}",
        label,
        description,
        control.checked ? "on" : "off"
      )
    )
    Panel(
      key: "authored.toggle.thumb",
      style: control.checked ? AuthoredThumbOn : AuthoredThumbOff
    )
  }
}

component AuthoredButtonPresentation(key: key, label: string, control: buttonState) {
  Scroll(key: key, layout: { width: 120, height: 28 }) {
    Panel(layout: { kind: "ROW", width: 120, height: 80 }) {
      Text(key: "authored.button.copy", text: format("{0}:{1}", label, control.enabled ? "on" : "off"))
    }
  }
}

component ForwardedAuthoredButtonPresentation(
  key: key,
  label: string,
  prefix: string,
  control: buttonState
) {
  Text(
    key: key,
    text: format("{0}{1}", prefix, label)
  )
}

component AuthoredButtonOwner(template: buttonTemplate) {
  Button(
    key: "authored.forwarded",
    label: "Apply",
    presentationTemplate: template
  )
}

component AuthoredIconButtonPresentation(
  key: key,
  label: string,
  icon: image,
  source: imageSource?,
  control: buttonState
) {
  Panel(key: key, layout: { kind: "PANEL", width: 32, height: 32 }) {
    Text(key: "authored.icon.copy", text: label)
  }
}

component AuthoredSliderPresentation(key: key, valueLabel: string, control: sliderState) {
  Panel(key: key, layout: { kind: "ROW", width: 180, height: 28 }) {
    Text(
      key: "authored.slider.copy",
      text: format("{0}:{1}:{2}", valueLabel, control.value, control.fraction)
    )
  }
}

component AuthoredProgressPresentation(key: key, control: progressState) {
  Panel(key: key, layout: { kind: "ROW", width: 180, height: 16 }) {
    Text(key: "authored.progress.copy", text: format("{0}", control.fraction))
  }
}

component AuthoredControls() {
  state sliderValue = 25;
  Panel(layout: { kind: "COLUMN", width: 220, height: "content", gap: 6 }) {
    Toggle(
      key: "authored.toggle",
      label: "Effects",
      description: "Use post-processing",
      defaultChecked: false,
      presentationTemplate: AuthoredTogglePresentation
    )
    Button(
      key: "authored.button",
      label: "Apply",
      presentationTemplate: AuthoredButtonPresentation
    )
    AuthoredButtonOwner(
      template: ForwardedAuthoredButtonPresentation(prefix: "Bound: ")
    )
    IconButton(
      key: "authored.icon",
      icon: "fixture:icon",
      label: "More",
      presentationTemplate: AuthoredIconButtonPresentation
    )
    Slider(
      key: "authored.slider",
      min: 0,
      max: 100,
      bind: sliderValue,
      presentationTemplate: AuthoredSliderPresentation(valueLabel: "Quarter")
    )
    Progress(
      key: "authored.progress",
      min: 0,
      max: 100,
      value: 40,
      presentationTemplate: AuthoredProgressPresentation
    )
    Panel(
      key: "authored.state-style",
      style: style(
        StateStyleBase,
        whenStyle(false, StateStyleMiddle),
        whenStyle(true, StateStyleMiddle),
        whenStyle(true, StateStyleFinal)
      ),
      layout: { width: 20, height: 10 }
    )
    Grid(
      key: "authored.grid",
      columns: [{ weight: 1 }],
      layout: { width: 20, height: 10 }
    ) {
      Panel(layout: { width: 4, height: 4 })
    }
  }
}

overlay Main { root AuthoredControls() }
)";

    strata::runtime::ApplicationContext application("authored-controls", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"authored-controls.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "authored control presentations did not compile and activate");

    strata::ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 320;
    environment.framebuffer_height = 260;
    environment.logical_width = 320.0;
    environment.logical_height = 260.0;
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
    strata::ui::Surface surface(
        "authored-controls",
        application,
        strata::runtime::LayerRole::overlay,
        "Main",
        environment,
        strata::ui::TextEngine::load_control_font(
            resource_root,
            strata::resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        )
    );
    const strata::ui::SurfaceFrame initial_frame = surface.frame(1'000'000);

    const strata::ui::RetainedNode* toggle = surface.tree().find_key("authored.toggle");
    const strata::ui::RetainedNode* presentation =
        surface.tree().find_key("authored.toggle.presentation");
    const strata::ui::RetainedNode* copy =
        surface.tree().find_key("authored.toggle.copy");
    const strata::ui::RetainedNode* thumb =
        surface.tree().find_key("authored.toggle.thumb");
    const strata::ui::LayoutRecord* thumb_layout =
        thumb != nullptr ? surface.layout().find(thumb->identity()) : nullptr;
    const strata::ui::LayoutRecord* toggle_layout =
        toggle != nullptr ? surface.layout().find(toggle->identity()) : nullptr;
    const strata::ui::LayoutRecord* presentation_layout =
        presentation != nullptr ? surface.layout().find(presentation->identity()) : nullptr;
    check(
        toggle != nullptr && presentation != nullptr && copy != nullptr &&
            thumb != nullptr && thumb_layout != nullptr && toggle_layout != nullptr &&
            presentation_layout != nullptr &&
            string_property(copy, "text") != nullptr &&
            *string_property(copy, "text") == "Effects:Use post-processing:off",
        "authored Toggle template did not receive its typed label, description, and state"
    );
    check(
        toggle_layout->bounds.width == 180.0 && toggle_layout->bounds.height == 28.0 &&
            presentation_layout->bounds.width == 180.0 &&
            presentation_layout->bounds.height == 28.0,
        "native authored-presentation owner did not derive its size from the template root"
    );
    check(
        surface.tree().find_key("authored.button.presentation") != nullptr &&
            surface.tree().find_key("authored.button.copy") != nullptr &&
            string_property(
                surface.tree().find_key("authored.forwarded.presentation"),
                "text"
            ) != nullptr &&
            *string_property(
                surface.tree().find_key("authored.forwarded.presentation"),
                "text"
            ) == "Bound: Apply" &&
            surface.tree().find_key("authored.icon.presentation") != nullptr &&
            surface.tree().find_key("authored.icon.copy") != nullptr &&
            surface.tree().find_key("authored.slider.presentation") != nullptr &&
            surface.tree().find_key("authored.slider.copy") != nullptr &&
            surface.tree().find_key("authored.progress.presentation") != nullptr &&
            string_property(
                surface.tree().find_key("authored.progress.copy"),
                "text"
            ) != nullptr &&
            *string_property(
                surface.tree().find_key("authored.progress.copy"),
                "text"
            ) == "0.4",
        "common authored control templates did not receive typed presentation state"
    );
    const strata::ui::RetainedNode* state_style =
        surface.tree().find_key("authored.state-style");
    const auto state_style_layout_property = state_style != nullptr
        ? state_style->description().properties.find("$layout")
        : strata::ui::DescriptionNode::Properties::const_iterator{};
    const strata::runtime::Value* state_style_layout =
        state_style != nullptr &&
            state_style_layout_property != state_style->description().properties.end()
        ? state_style_layout_property->second.value()
        : nullptr;
    const strata::runtime::Value* state_style_radius =
        state_style_layout != nullptr
            ? state_style_layout->field("radius")
            : nullptr;
    check(
        state_style_radius != nullptr &&
            state_style_radius->number() != nullptr &&
            *state_style_radius->number() == 9.0,
        "conditional state-style layers did not skip false state or preserve override order"
    );
    const strata::ui::RetainedNode* grid =
        surface.tree().find_key("authored.grid");
    const strata::ui::LayoutRecord* grid_layout =
        grid != nullptr ? surface.layout().find(grid->identity()) : nullptr;
    check(
        grid_layout != nullptr && grid_layout->kind == strata::ui::LayoutKind::grid,
        "Grid did not apply its implicit GRID layout kind"
    );
    check(
        std::ranges::none_of(
            initial_frame.diagnostics.records,
            [](const strata::runtime::RuntimeDiagnosticRecord& record) {
                return record.diagnostic.code ==
                    "STRATA.UI.SEMANTICS_FOCUSABLE_INVISIBLE";
            }
        ),
        "presentation-only descendants emitted hidden-focus semantic diagnostics"
    );
    const strata::ui::RetainedNode* button =
        surface.tree().find_key("authored.button");
    const strata::ui::RetainedNode* button_presentation =
        surface.tree().find_key("authored.button.presentation");
    const strata::ui::LayoutRecord* button_layout =
        button != nullptr ? surface.layout().find(button->identity()) : nullptr;
    const strata::ui::LayoutRecord* button_presentation_layout =
        button_presentation != nullptr
            ? surface.layout().find(button_presentation->identity())
            : nullptr;
    check(
        button_layout != nullptr && button_presentation_layout != nullptr &&
            button_presentation_layout->kind == strata::ui::LayoutKind::scroll &&
            button_presentation_layout->content_size.height >
                button_presentation_layout->content_bounds.height,
        "authored presentation input-transparency fixture is not scrollable"
    );
    static_cast<void>(surface.input().enqueue_scroll(strata::ui::ScrollInputEvent{
        strata::ui::Point{
            button_layout->bounds.x + button_layout->bounds.width * 0.5,
            button_layout->bounds.y + button_layout->bounds.height * 0.5,
        },
        0.0,
        -1.0,
    }));
    static_cast<void>(surface.frame(1'500'000));
    button_presentation = surface.tree().find_key("authored.button.presentation");
    button_presentation_layout = button_presentation != nullptr
        ? surface.layout().find(button_presentation->identity())
        : nullptr;
    check(
        button_presentation_layout != nullptr &&
            button_presentation_layout->scroll_offset.y == 0.0,
        "presentation-only descendant consumed wheel input owned by the native control"
    );
    const strata::ui::Rect presented_thumb = strata::ui::transform_presentation_bounds(
        thumb_layout->bounds,
        strata::ui::local_presentation_transform(
            *thumb,
            surface.motion(),
            thumb_layout->bounds
        )
    );
    check(
        std::abs(presented_thumb.width - 6.0) < 0.001 &&
            std::abs(presented_thumb.height - 6.0) < 0.001 &&
            std::abs(
                presented_thumb.x + presented_thumb.width * 0.5 -
                (thumb_layout->bounds.x + thumb_layout->bounds.width * 0.5)
            ) < 0.001 &&
            std::abs(
                presented_thumb.y + presented_thumb.height * 0.5 -
                (thumb_layout->bounds.y + thumb_layout->bounds.height * 0.5)
            ) < 0.001,
        "presentation scale moved authored content away from its own center"
    );
    const strata::data::JsonValue* semantics = surface.semantics().find(toggle->identity());
    const strata::data::JsonValue* role = semantics != nullptr ? semantics->find("role") : nullptr;
    check(
        role != nullptr && role->string() != nullptr && *role->string() == "switch",
        "authored Toggle presentation lost native switch semantics"
    );

    static_cast<void>(surface.input().click("authored.toggle"));
    static_cast<void>(surface.frame(2'000'000));
    copy = surface.tree().find_key("authored.toggle.copy");
    check(
        copy != nullptr && string_property(copy, "text") != nullptr &&
            *string_property(copy, "text") == "Effects:Use post-processing:on",
        "authored Toggle presentation did not rematerialize after native activation"
    );
    static_cast<void>(surface.input().click("authored.slider"));
    static_cast<void>(surface.frame(3'000'000));
    const std::string* slider_copy = string_property(
        surface.tree().find_key("authored.slider.copy"),
        "text"
    );
    check(
        slider_copy != nullptr && slider_copy->starts_with("Quarter:50:0.5"),
        "authored Slider presentation did not track native pointer state"
    );
}

void test_section_content_and_activation_contract(
    const std::filesystem::path& resource_root,
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
    const std::filesystem::path& resources = resource_root;
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
    const std::filesystem::path& resource_root,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
style TransparentPresentation {
  background: null;
  border: null;
}

style ActiveChoicePresentation extends TransparentPresentation {
  opacity: 0.45;
}

component InteractiveChoiceTrigger(
  key: key,
  label: string,
  enabled: boolean,
  expanded: boolean
) {
  Button(
    key: key,
    label: label,
    enabled: enabled,
    layout: { width: 180, height: 32 }
  )
}

component InteractiveChoicePopup(key: key, level: number, expanded: boolean) {
  Panel(
    key: key,
    style: TransparentPresentation,
    layout: { kind: "COLUMN", width: 260, height: "content" }
  )
}

component InteractiveChoiceItem(
  key: key,
  id: string,
  label: string,
  index: number,
  enabled: boolean,
  selected: boolean,
  active: boolean
) {
  Panel(
    key: key,
    style: active ? ActiveChoicePresentation : TransparentPresentation
  ) {
    Text(text: label)
  }
}

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
    Select(
      key: "defaults.custom-select",
      options: [
        { id: "custom", label: "Custom" },
        { id: "alternate", label: "Alternate" }
      ],
      expanded: true,
      triggerTemplate: InteractiveChoiceTrigger,
      popupTemplate: InteractiveChoicePopup,
      itemTemplate: InteractiveChoiceItem
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
    const std::filesystem::path& resources = resource_root;
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
    const strata::ui::RetainedNode* custom_popup =
        surface.tree().find_key("defaults.custom-select.popup");
    const strata::ui::RetainedNode* custom_popup_surface =
        surface.tree().find_key("defaults.custom-select.popup.surface");
    const strata::ui::LayoutRecord* custom_popup_layout = custom_popup != nullptr
        ? surface.layout().find(custom_popup->identity())
        : nullptr;
    const strata::ui::LayoutRecord* custom_popup_surface_layout =
        custom_popup_surface != nullptr
            ? surface.layout().find(custom_popup_surface->identity())
            : nullptr;
    check(
        custom_popup_layout != nullptr && custom_popup_surface_layout != nullptr &&
            custom_popup_layout->bounds.width == 260.0 &&
            custom_popup_surface_layout->bounds.width == 260.0,
        "authored Select popup did not retain its template-owned width"
    );
    const bool wrapper_drew_theme_chrome = std::ranges::any_of(
        surface.render_commands().commands(),
        [custom_popup_layout](const strata::ui::RenderCommand& command) {
            if (const auto* rounded =
                    std::get_if<strata::ui::RoundedRectRenderCommand>(&command);
                rounded != nullptr) {
                return rounded->bounds == custom_popup_layout->bounds;
            }
            const auto* border =
                std::get_if<strata::ui::BorderRenderCommand>(&command);
            return border != nullptr && border->bounds == custom_popup_layout->bounds;
        }
    );
    check(
        !wrapper_drew_theme_chrome,
        "transparent authored-popup wrapper inherited default theme chrome"
    );
    const strata::ui::RetainedNode* custom_select =
        surface.tree().find_key("defaults.custom-select");
    const std::vector<strata::ui::WidgetSubtarget> custom_targets =
        surface.input().subtargets(custom_select->identity());
    const auto alternate = std::ranges::find_if(
        custom_targets,
        [](const strata::ui::WidgetSubtarget& target) {
            return target.kind == strata::ui::WidgetSubtargetKind::choice &&
                target.index == 1U;
        }
    );
    check(
        alternate != custom_targets.end(),
        "authored Select did not publish its alternate option hit target"
    );
    static_cast<void>(surface.input().enqueue_pointer(strata::ui::PointerInputEvent{
        {
            alternate->bounds.x + alternate->bounds.width * 0.5,
            alternate->bounds.y + alternate->bounds.height * 0.5,
        },
        strata::ui::PointerEventType::move,
        0,
        0,
    }));
    const strata::ui::SurfaceFrame hover_frame = surface.frame(1'500'000);
    const strata::ui::RetainedNode* alternate_row =
        surface.tree().find_key("defaults.custom-select.option.alternate");
    const strata::runtime::Value* alternate_layout = nullptr;
    if (alternate_row != nullptr) {
        const auto layout = alternate_row->description().properties.find("$layout");
        if (layout != alternate_row->description().properties.end()) {
            alternate_layout = layout->second.value();
        }
    }
    const strata::runtime::Value* alternate_opacity =
        alternate_layout != nullptr ? alternate_layout->field("opacity") : nullptr;
    check(
        hover_frame.operations.rebuilds == 1U &&
            alternate_opacity != nullptr && alternate_opacity->number() != nullptr &&
            *alternate_opacity->number() == 0.45,
        "authored Select hover did not rematerialize its active item on pointer movement"
    );
    static_cast<void>(surface.input().click("defaults.select"));
    static_cast<void>(surface.input().key("h"));
    static_cast<void>(surface.input().key("enter"));
    static_cast<void>(surface.frame(2'000'000));
    select = surface.tree().find_key("defaults.select");
    check(
        string_property(select, "selectedId") != nullptr &&
            *string_property(select, "selectedId") == "quality",
        "Select typeahead did not move and commit the matching option"
    );
    static_cast<void>(surface.input().click("defaults.custom-select"));
    static_cast<void>(surface.input().key("tab"));
    check(
        !surface.input().focused_key().has_value() ||
            *surface.input().focused_key() != "defaults.custom-select.trigger",
        "authored Select presentation introduced a competing keyboard focus target"
    );
    static_cast<void>(surface.dispatch_action(
        "focus.request",
        strata::runtime::Value(
            std::vector<std::pair<std::string, strata::runtime::Value>>{
                {"key", strata::runtime::Value(strata::runtime::KeyValue{
                    "defaults.custom-select.trigger"
                })},
            }
        ),
        "test",
        std::nullopt,
        strata::runtime::Value{}
    ));
    check(
        !surface.input().focused_key().has_value() ||
            *surface.input().focused_key() != "defaults.custom-select.trigger",
        "programmatic focus escaped into authored Select presentation"
    );
    // The preceding state changes may reconcile the component subtree. Retained identities are
    // stable across that work; raw node/layout addresses are frame-local observations.
    radio = surface.tree().find_key("defaults.radio");
    radio_layout = radio != nullptr
        ? surface.layout().find(radio->identity())
        : nullptr;
    check(
        radio != nullptr && radio_layout != nullptr,
        "RadioGroup disappeared after sibling Select reconciliation"
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
    const std::filesystem::path& resource_root,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
style RoundedCachePanel {
  width: 240;
  height: 160;
  clip: true;
  radius: 12;
}
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
    Panel(key: "cache.bottom", style: RoundedCachePanel) {
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
    const std::filesystem::path& resources = resource_root;
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
    check(
        bottom_layout->local_clip.has_value(),
        "scrolled clipped subtree lost its raw local clip"
    );
    strata::ui::Point translation;
    strata::ui::Rect effective_clip{0.0, 0.0, 320.0, 180.0};
    std::vector<strata::ui::Point> translation_stack;
    std::vector<strata::ui::Rect> clip_stack;
    bool emitted_current_clip = false;
    bool emitted_rounded_current_clip = false;
    bool reused_translated_clip = false;
    for (const strata::ui::RenderCommand& command : surface.render_commands().commands()) {
        if (const auto* transform_push =
                std::get_if<strata::ui::TransformPushRenderCommand>(&command);
            transform_push != nullptr) {
            check(
                transform_push->m00 == 1.0 && transform_push->m01 == 0.0 &&
                    transform_push->m10 == 0.0 && transform_push->m11 == 1.0,
                "scroll clip-cache fixture unexpectedly emitted a non-translation transform"
            );
            translation_stack.push_back(translation);
            translation.x += transform_push->m02;
            translation.y += transform_push->m12;
        } else if (std::holds_alternative<strata::ui::TransformPopRenderCommand>(command)) {
            check(!translation_stack.empty(), "scroll clip-cache transform stack underflow");
            translation = translation_stack.back();
            translation_stack.pop_back();
        } else if (const auto* clip_push =
                       std::get_if<strata::ui::ClipPushRenderCommand>(&command);
                   clip_push != nullptr) {
            clip_stack.push_back(effective_clip);
            const strata::ui::Rect translated{
                clip_push->rect.x + translation.x,
                clip_push->rect.y + translation.y,
                clip_push->rect.width,
                clip_push->rect.height,
            };
            effective_clip = effective_clip.clip_intersection(translated);
            if (translated == *bottom_layout->local_clip) {
                emitted_current_clip = effective_clip == *bottom_layout->clip;
                emitted_rounded_current_clip =
                    clip_push->radii == strata::ui::CornerRadii::all(12.0);
                reused_translated_clip = translation != strata::ui::Point{};
            }
        } else if (std::holds_alternative<strata::ui::ClipPopRenderCommand>(command)) {
            check(!clip_stack.empty(), "scroll clip-cache clip stack underflow");
            effective_clip = clip_stack.back();
            clip_stack.pop_back();
        }
    }
    check(
        translation_stack.empty() && clip_stack.empty(),
        "scroll clip-cache fixture left render state unbalanced"
    );
    check(
        emitted_current_clip,
        "retained subtree translation did not recompute the visible clip intersection"
    );
    check(
        emitted_rounded_current_clip,
        "layout clipping discarded the authored descendant corner radius"
    );
    check(
        reused_translated_clip,
        "clip-cache regression recomposed instead of exercising subtree translation reuse"
    );

    constexpr std::string_view scaled_source = R"(
overlay Main {
  root Scroll(
    key: "scaled.scroll",
    vertical: true,
    horizontal: false,
    layout: { kind: "SCROLL", width: 280, height: 120 },
    style: { scale: 2 }
  ) {
    Panel(key: "scaled.top", layout: { width: 240, height: 160, clip: true }) {
      Text(text: "Scaled top clipped content")
    }
    Panel(key: "scaled.bottom", layout: { width: 220, height: 160, clip: true }) {
      Text(text: "Scaled bottom clipped content")
    }
  }
}
)";
    strata::runtime::ApplicationContext scaled_application("scaled-scroll-clip-cache", bundle);
    const strata::runtime::ActivationResult scaled_activation =
        scaled_application.compile_and_activate(
            strata::compiler::ModuleSource{
                "scaled-scroll-clip-cache.strata", std::string(scaled_source),
            },
            no_imports(),
            0U
        );
    check(scaled_activation.activated(), "scaled scroll clip-cache fixture did not activate");
    strata::ui::Surface scaled_surface(
        "scaled-scroll-clip-cache",
        scaled_application,
        strata::runtime::LayerRole::overlay,
        "Main",
        environment,
        strata::ui::TextEngine::load_control_font(
            resources,
            strata::resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        )
    );
    static_cast<void>(scaled_surface.frame(1'000'000));
    check(
        scaled_surface.animate_scroll_to(strata::ui::ScrollAnimationRequest{
            "scaled.scroll", std::nullopt, 160.0, "standard", std::nullopt,
        }),
        "scaled scroll clip-cache fixture did not accept a scroll offset"
    );
    static_cast<void>(scaled_surface.frame(2'000'000));
    const strata::ui::RetainedNode* scaled_scroll =
        scaled_surface.tree().find_key("scaled.scroll");
    const strata::ui::RetainedNode* scaled_bottom =
        scaled_surface.tree().find_key("scaled.bottom");
    const strata::ui::LayoutRecord* scaled_scroll_layout = scaled_scroll != nullptr
        ? scaled_surface.layout().find(scaled_scroll->identity())
        : nullptr;
    const strata::ui::LayoutRecord* scaled_bottom_layout = scaled_bottom != nullptr
        ? scaled_surface.layout().find(scaled_bottom->identity())
        : nullptr;
    check(
        scaled_scroll != nullptr && scaled_scroll_layout != nullptr &&
            scaled_bottom_layout != nullptr && scaled_bottom_layout->local_clip.has_value(),
        "scaled clipped subtree lost its raw local clip"
    );
    const strata::ui::Rect expected_scaled_clip = strata::ui::inverse_presentation_bounds(
        *scaled_bottom_layout->local_clip,
        strata::ui::local_presentation_transform(
            *scaled_scroll,
            scaled_surface.motion(),
            scaled_scroll_layout->bounds
        )
    );
    const bool emitted_recomposed_scaled_clip = std::ranges::any_of(
        scaled_surface.render_commands().commands(),
        [&expected_scaled_clip](const strata::ui::RenderCommand& command) {
            const auto* clip = std::get_if<strata::ui::ClipPushRenderCommand>(&command);
            return clip != nullptr && clip->rect == expected_scaled_clip;
        }
    );
    check(
        emitted_recomposed_scaled_clip,
        "scaled clipped subtree incorrectly reused a clip with scaled translation"
    );
}

void test_typed_host_keys_and_derived_collection_metadata() {
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
    const auto bundle = strata::runtime::ApplicationBundle::create(&schemas);
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
        if (argument_count != 2) throw std::invalid_argument("expected resource root");
        const std::filesystem::path resource_root(arguments[1]);
        const auto bundle = load_bundle();
        test_semantic_binding_rejections(bundle);
        test_binding_initial_value_and_round_trip(resource_root, bundle);
        test_authored_control_presentations(resource_root, bundle);
        test_section_content_and_activation_contract(resource_root, bundle);
        test_range_and_choice_control_defaults(resource_root, bundle);
        test_scrolled_clipped_subtree_render_cache(resource_root, bundle);
        test_typed_host_keys_and_derived_collection_metadata();
        std::cout << "strata_widget_binding_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_widget_binding_tests: " << error.what() << '\n';
        return 1;
    }
}
