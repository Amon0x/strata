#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/source.hpp"
#include "data/json.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "ui/surface.hpp"
#include "ui/text.hpp"
#include "ui/widget/presentation.hpp"

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
            "unexpected command fixture import '" + std::string(path) + "'"
        );
    };
}

[[nodiscard]] bool contains_field_value(
    const strata::data::JsonValue& value,
    const std::string_view field,
    const std::string_view expected
) {
    if (value.object() != nullptr) {
        if (const strata::data::JsonValue* child = value.find(field);
            child != nullptr && child->string() != nullptr && *child->string() == expected) {
            return true;
        }
        for (const auto& [name, child] : *value.object()) {
            static_cast<void>(name);
            if (contains_field_value(child, field, expected)) return true;
        }
    }
    if (value.array() != nullptr) {
        for (const strata::data::JsonValue& child : *value.array()) {
            if (contains_field_value(child, field, expected)) return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint8_t background_alpha(
    const strata::ui::Surface& surface,
    const strata::ui::RetainedNode& node
) {
    const strata::ui::LayoutRecord* layout = surface.layout().find(node.identity());
    check(layout != nullptr, "command-bound control was not laid out");
    const std::vector<strata::ui::RenderCommand> fragment = strata::ui::build_widget_fragment(
        surface.widget_registry(),
        node,
        *layout,
        surface.layout(),
        surface.input(),
        surface.commands(),
        surface.text_engine(),
        nullptr,
        &surface.motion(),
        1.0
    );
    for (const strata::ui::RenderCommand& command : fragment) {
        if (const auto* rectangle = std::get_if<strata::ui::RoundedRectRenderCommand>(&command);
            rectangle != nullptr) {
            return rectangle->fill.representative().alpha;
        }
    }
    throw std::runtime_error("command-bound Button did not render its background");
}

[[nodiscard]] bool outcomes_contain(
    const strata::ui::InputOperationResult& result,
    const std::string_view action_id
) {
    for (const strata::data::JsonValue& outcome : result.action_outcomes) {
        if (contains_field_value(outcome, "actionId", action_id)) return true;
    }
    return false;
}

void test_command_bound_controls(
    const std::filesystem::path& registry_path,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component CommandFixture() {
  Panel(key: "command.root", layout: { kind: "COLUMN", width: { weight: 1 }, height: { weight: 1 }, gap: 8 }) {
    Command(
      id: "command.save", label: "Save", shortcut: "Ctrl+S",
      action: action("form.submit", key: "command.form")
    )
    Command(
      id: "command.icon", label: "Icon action",
      action: action("notification.raise", message: "Icon command executed", severity: "INFO")
    )
    Command(
      id: "command.disabled", label: "Disabled", enabled: false,
      action: action("notification.raise", message: "Disabled command executed", severity: "ERROR")
    )
    Form(
      key: "command.form",
      onSubmit: action("notification.raise", message: "Form command submitted", severity: "SUCCESS")
    ) {
      Button(key: "command.button", label: "Command-bound save", command: "command.save")
    }
    IconButton(
      key: "command.icon-button", icon: "fixture:icon", label: "Icon command",
      command: "command.icon"
    )
    Button(
      key: "command.disabled-button", label: "Disabled command",
      command: "command.disabled"
    )
    Button(
      key: "command.unknown-button", label: "Unknown command",
      command: "command.missing"
    )
  }
}
overlay Main { root CommandFixture() }
)";

    strata::runtime::ApplicationContext application("command-binding", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"command-binding.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "command-bound control fixture did not activate");

    strata::ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 720;
    environment.framebuffer_height = 520;
    environment.logical_width = 720.0;
    environment.logical_height = 520.0;
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
        "command-binding",
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

    const strata::ui::RetainedNode* button = surface.tree().find_key("command.button");
    const strata::ui::RetainedNode* icon = surface.tree().find_key("command.icon-button");
    const strata::ui::RetainedNode* disabled = surface.tree().find_key("command.disabled-button");
    check(button != nullptr && icon != nullptr && disabled != nullptr,
          "command-bound controls were not retained");

    const auto button_binding = surface.commands().activation_binding(*button);
    const auto icon_binding = surface.commands().activation_binding(*icon);
    const auto disabled_binding = surface.commands().activation_binding(*disabled);
    check(
        button_binding.has_value() && button_binding->command != nullptr &&
            button_binding->id == "command.save" &&
        icon_binding.has_value() && icon_binding->command != nullptr &&
            icon_binding->id == "command.icon" &&
        disabled_binding.has_value() && disabled_binding->command != nullptr &&
            !disabled_binding->command->enabled,
        "Button/IconButton command references did not resolve through the shared command index"
    );
    check(
        surface.commands().referenced_by(*button).size() == 1U &&
            surface.commands().referenced_by(*icon).size() == 1U,
        "command-bound controls did not expose resolved command metadata"
    );
    check(
        strata::ui::format_command_shortcut(*button_binding->command) == "Ctrl+S",
        "command shortcut formatting diverged from command surface presentation"
    );

    const strata::ui::LayoutRecord* button_layout = surface.layout().find(button->identity());
    check(button_layout != nullptr, "command-bound Button was not arranged for hover disclosure");
    const strata::ui::Point button_center{
        button_layout->bounds.x + button_layout->bounds.width * 0.5,
        button_layout->bounds.y + button_layout->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue_pointer(strata::ui::PointerInputEvent{
        button_center, strata::ui::PointerEventType::move, 0, 0,
    }));
    const strata::ui::SurfaceFrame hover_started = surface.frame(20'000'000);
    const strata::ui::SurfaceFrame hover_early = surface.frame(469'999'999);
    check(
        hover_started.operations.render.overlays_rendered == 0U &&
            hover_early.operations.render.overlays_rendered == 0U,
        "command tooltip appeared before the deterministic 450ms hover deadline"
    );
    const strata::ui::SurfaceFrame hover_matured = surface.frame(470'000'000);
    check(
        hover_matured.operations.render.overlays_rendered == 1U &&
            surface.input().command_tooltip_ready(button->identity()),
        "command-bound Button did not render its delayed detached command tooltip"
    );
    const strata::ui::SurfaceFrame hover_settled = surface.frame(486'666'667);
    check(
        hover_settled.operations.render.nodes_visited == 0U,
        "mature command tooltip kept an idle surface in a perpetual frame loop"
    );
    static_cast<void>(surface.input().enqueue_pointer(strata::ui::PointerInputEvent{
        strata::ui::Point{button_center.x + 0.25, button_center.y},
        strata::ui::PointerEventType::move,
        0,
        0,
    }));
    static_cast<void>(surface.frame(503'333'334));
    static_cast<void>(surface.input().enqueue_pointer(strata::ui::PointerInputEvent{
        strata::ui::Point{button_center.x + 0.5, button_center.y},
        strata::ui::PointerEventType::move,
        0,
        0,
    }));
    const strata::ui::SurfaceFrame cached_pointer = surface.frame(520'000'001);
    check(
        cached_pointer.operations.input_events_processed == 1U &&
            cached_pointer.operations.pointer_geometry_rebuilds == 0U &&
            cached_pointer.operations.input_fast_path_frames == 1U &&
            cached_pointer.operations.render.nodes_visited == 0U,
        "stable pointer motion rebuilt retained geometry or traversed rendering"
    );

    const strata::ui::InputOperationResult submitted = surface.input().click("command.button");
    bool form_submit = false;
    bool notification = false;
    bool fallback_button = false;
    for (const strata::data::JsonValue& outcome : submitted.action_outcomes) {
        form_submit = form_submit || contains_field_value(outcome, "actionId", "form.submit");
        notification = notification ||
                       contains_field_value(outcome, "actionId", "notification.raise");
        fallback_button = fallback_button || contains_field_value(outcome, "actionId", "Button");
    }
    check(
        form_submit && notification && !fallback_button,
        "command-bound Button did not execute the registered command through Form submission"
    );

    const strata::ui::InputOperationResult icon_result = surface.input().click(
        "command.icon-button"
    );
    check(
        outcomes_contain(icon_result, "notification.raise"),
        "command-bound IconButton did not execute its registered command"
    );

    const std::uint8_t enabled_alpha = background_alpha(surface, *button);
    const std::uint8_t disabled_alpha = background_alpha(surface, *disabled);
    const strata::ui::InputOperationResult disabled_result = surface.input().click(
        "command.disabled-button"
    );
    check(
        !outcomes_contain(disabled_result, "notification.raise") &&
            !outcomes_contain(disabled_result, "Button") &&
            std::ranges::none_of(disabled_result.events, [](const strata::data::JsonValue& event) {
                return contains_field_value(event, "type", "activated");
            }) &&
            !surface.input().focused_key().has_value() &&
            disabled_alpha < enabled_alpha,
        "disabled command state activated, retained stale focus, or lost presentation state"
    );
    const strata::data::JsonValue* disabled_semantics = surface.semantics().find(
        disabled->identity()
    );
    check(
        disabled_semantics != nullptr &&
            disabled_semantics->find("state") != nullptr &&
            disabled_semantics->find("state")->find("disabled") != nullptr &&
            disabled_semantics->find("state")->find("disabled")->boolean() != nullptr &&
            *disabled_semantics->find("state")->find("disabled")->boolean(),
        "disabled command state did not propagate into control semantics"
    );

    const strata::ui::InputOperationResult unknown = surface.input().click(
        "command.unknown-button"
    );
    bool unknown_status = false;
    bool unknown_fell_back = false;
    for (const strata::data::JsonValue& event : unknown.events) {
        unknown_status = unknown_status ||
                         contains_field_value(event, "commandStatus", "unknown");
        unknown_fell_back = unknown_fell_back || contains_field_value(event, "id", "Button");
    }
    check(
        unknown_status && !unknown_fell_back,
        "unknown command binding fell through to the generic Button observer action"
    );
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2) throw std::invalid_argument("expected registry path");
        const std::filesystem::path registry_path(arguments[1]);
        const auto bundle = load_bundle(registry_path);
        test_command_bound_controls(registry_path, bundle);
        std::cout << "strata_command_binding_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_command_binding_tests: " << error.what() << '\n';
        return 1;
    }
}
