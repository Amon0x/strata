#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/source.hpp"
#include "data/json.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "ui/render.hpp"
#include "ui/surface.hpp"
#include "ui/text.hpp"

namespace {

using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::shared_ptr<const runtime::ApplicationBundle> load_bundle() {
    return runtime::ApplicationBundle::create();
}

[[nodiscard]] compiler::ModuleLoader no_imports() {
    return [](const std::string_view, const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError(
            "unexpected ChipInput fixture import '" + std::string(path) + "'"
        );
    };
}

[[nodiscard]] ui::SurfaceEnvironment environment() {
    ui::SurfaceEnvironment result;
    result.framebuffer_width = 640;
    result.framebuffer_height = 240;
    result.logical_width = 640.0;
    result.logical_height = 240.0;
    result.reduced_motion = true;
    result.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, false,
    };
    return result;
}

[[nodiscard]] std::shared_ptr<const ui::TextEngine> text_engine(
    const std::filesystem::path& resource_root
) {
    return ui::TextEngine::load_control_font(
        resource_root,
        resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
    );
}

[[nodiscard]] std::vector<std::string> strings(const runtime::Value* value) {
    std::vector<std::string> result;
    if (value == nullptr || value->list() == nullptr) return result;
    for (const runtime::Value& item : value->list()->values) {
        if (item.string() != nullptr) result.push_back(*item.string());
    }
    return result;
}

[[nodiscard]] const runtime::Value* property(
    const ui::RetainedNode& node,
    const std::string_view name
) {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] const ui::WidgetSubtarget* target(
    const std::vector<ui::WidgetSubtarget>& targets,
    const std::string_view id
) {
    const auto found = std::ranges::find(targets, id, &ui::WidgetSubtarget::id);
    return found != targets.end() ? &*found : nullptr;
}

void test_uncontrolled_editor_lifecycle(
    const std::filesystem::path& resource_root,
    const std::shared_ptr<const runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component ChipFixture() {
  ChipInput(
    key: "chips",
    defaultValues: ["alpha"],
    placeholder: "Add a tag",
    maxChips: 3
  )
}
overlay Main { root ChipFixture() }
)";
    runtime::ApplicationContext application("chip-uncontrolled", bundle);
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"chip-uncontrolled.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "uncontrolled ChipInput fixture did not activate");
    ui::Surface surface(
        "chip-uncontrolled", application, runtime::LayerRole::overlay, "Main",
        environment(), text_engine(resource_root)
    );
    static_cast<void>(surface.frame(1'000'000));
    ui::RetainedNode* chip = surface.tree().find_key("chips");
    check(chip != nullptr, "ChipInput was not retained");
    check(
        surface.input().editor_snapshot(chip->identity()).has_value(),
        "ChipInput did not install a shared text editor"
    );

    const std::vector<ui::WidgetSubtarget> initial_targets =
        surface.input().subtargets(chip->identity());
    const ui::WidgetSubtarget* alpha = target(initial_targets, "token:0");
    const ui::WidgetSubtarget* editor = target(initial_targets, "$editor");
    check(alpha != nullptr && editor != nullptr, "ChipInput omitted token/editor subtargets");
    check(
        alpha->bounds.right() <= editor->bounds.x,
        "ChipInput token and editor geometry disagree"
    );

    const auto pointer_click = [&surface](const ui::Rect bounds) {
        const ui::Point point{
            bounds.x + bounds.width * 0.5,
            bounds.y + bounds.height * 0.5,
        };
        static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
            point, ui::PointerEventType::press, 7, 0,
        }));
        static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
            point, ui::PointerEventType::release, 7, 0,
        }));
        return surface.input().process_queued();
    };
    static_cast<void>(pointer_click(alpha->bounds));
    const runtime::Value* pointer_active = chip->retained_value("$activeToken");
    check(
        pointer_active != nullptr && pointer_active->number() != nullptr &&
            *pointer_active->number() == 0.0,
        "pointer token subtarget did not select its token"
    );
    static_cast<void>(pointer_click(editor->bounds));
    pointer_active = chip->retained_value("$activeToken");
    check(
        pointer_active == nullptr || pointer_active->number() == nullptr,
        "pointer editor subtarget did not return ownership to the draft"
    );
    check(
        surface.input().focused(chip->identity()),
        "ChipInput did not accept editor focus"
    );
    const ui::InputOperationResult partial = surface.input().text(" beta ,gam");
    const std::optional<ui::TextEditorSnapshot> partial_draft =
        surface.input().editor_snapshot(chip->identity());
    check(
        strings(chip->retained_value("$values")) ==
            std::vector<std::string>{"alpha", "beta"} &&
            partial.events.size() == 1U && partial_draft.has_value() &&
            partial_draft->text == "gam",
        "text after the final delimiter was not preserved as the next draft"
    );
    const ui::InputOperationResult committed = surface.input().text("ma;delta,");
    check(
        strings(chip->retained_value("$values")) ==
            std::vector<std::string>{"alpha", "beta", "gamma"},
        "delimiter commits did not trim, append, and enforce maxChips"
    );
    check(
        committed.events.size() == 1U &&
            committed.events.back().find("type") != nullptr &&
            committed.events.back().find("type")->string() != nullptr &&
            *committed.events.back().find("type")->string() == "values-changed",
        "ChipInput did not emit one complete values event per accepted token"
    );
    const std::optional<ui::TextEditorSnapshot> empty =
        surface.input().editor_snapshot(chip->identity());
    check(empty.has_value() && empty->text.empty(), "delimiter commit left stale draft text");

    static_cast<void>(surface.input().key("left"));
    const runtime::Value* active = chip->retained_value("$activeToken");
    check(
        active != nullptr && active->number() != nullptr && *active->number() == 2.0,
        "left from an empty draft did not select the trailing token"
    );
    static_cast<void>(surface.frame(2'000'000));
    const std::vector<ui::WidgetSubtarget> selected_targets =
        surface.input().subtargets(chip->identity());
    const ui::WidgetSubtarget* selected = target(selected_targets, "token:2");
    check(
        selected != nullptr && selected->bounds.x >= surface.layout().find(chip->identity())->bounds.x &&
            selected->bounds.right() <= surface.layout().find(chip->identity())->bounds.right(),
        "keyboard-selected overflow token was not kept in the visible token lane"
    );
    check(
        std::ranges::any_of(surface.render_commands().commands(), [](const ui::RenderCommand& command) {
            const auto* rounded = std::get_if<ui::RoundedRectRenderCommand>(&command);
            return rounded != nullptr && rounded->fill == ui::Paint(ui::RenderColor{91U, 141U, 239U, 175U});
        }),
        "active ChipInput token was not projected by the presenter"
    );

    const ui::InputOperationResult removed = surface.input().key("delete");
    check(
        strings(chip->retained_value("$values")) ==
            std::vector<std::string>{"alpha", "beta"} && !removed.events.empty(),
        "delete did not remove and publish the selected token"
    );
    const ui::InputOperationResult draft = surface.input().text("draft");
    const std::optional<ui::TextEditorSnapshot> edited =
        surface.input().editor_snapshot(chip->identity());
    check(
        draft.events.empty() && edited.has_value() && edited->text == "draft",
        "draft editing escaped through the token-list onChange contract"
    );
    static_cast<void>(surface.frame(3'000'000));
    const std::vector<ui::WidgetSubtarget> draft_targets =
        surface.input().subtargets(chip->identity());
    const ui::WidgetSubtarget* current_editor = target(draft_targets, "$editor");
    check(current_editor != nullptr, "ChipInput editor geometry disappeared after editing");
    check(
        std::ranges::any_of(surface.render_commands().commands(), [current_editor](
            const ui::RenderCommand& command
        ) {
            const auto* caret = std::get_if<ui::SolidRectRenderCommand>(&command);
            return caret != nullptr && std::abs(caret->bounds.width - 1.0) < 0.001 &&
                caret->bounds.x >= current_editor->bounds.x &&
                caret->bounds.x <= current_editor->bounds.right();
        }),
        "focused ChipInput draft did not render its shared editor caret"
    );
    static_cast<void>(surface.input().key("enter"));
    check(
        strings(chip->retained_value("$values")) ==
            std::vector<std::string>{"alpha", "beta", "draft"},
        "enter did not commit the shared editor draft"
    );

    static_cast<void>(surface.frame(4'000'000));
    const data::JsonValue* semantics = surface.semantics().find(chip->identity());
    const data::JsonValue* state = semantics != nullptr ? semantics->find("state") : nullptr;
    const data::JsonValue* value_text = state != nullptr ? state->find("valueText") : nullptr;
    const data::JsonValue* children = semantics != nullptr ? semantics->find("children") : nullptr;
    check(
        value_text != nullptr && value_text->string() != nullptr &&
            *value_text->string() == "alpha, beta, draft" &&
            children != nullptr && children->array() != nullptr && children->array()->size() == 3U,
        "ChipInput semantics omitted its value summary or virtual tokens"
    );
}

void test_controlled_binding_round_trip(
    const std::filesystem::path& resource_root,
    const std::shared_ptr<const runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component BoundChips() {
  state tags = ["seed"];
  ChipInput(key: "bound.chips", bind: tags, maxChips: 4)
}
overlay Main { root BoundChips() }
)";
    runtime::ApplicationContext application("chip-controlled", bundle);
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"chip-controlled.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "controlled ChipInput fixture did not activate");
    ui::Surface surface(
        "chip-controlled", application, runtime::LayerRole::overlay, "Main",
        environment(), text_engine(resource_root)
    );
    static_cast<void>(surface.frame(1'000'000));
    ui::RetainedNode* chip = surface.tree().find_key("bound.chips");
    check(chip != nullptr, "bound ChipInput was not retained");
    static_cast<void>(surface.input().click("bound.chips"));
    const ui::InputOperationResult changed = surface.input().text("next,");
    check(!changed.action_outcomes.empty(), "controlled ChipInput did not dispatch onChange");
    check(
        chip->retained_value("$values") == nullptr,
        "controlled ChipInput wrote an uncontrolled token-list shadow"
    );
    static_cast<void>(surface.frame(2'000'000));
    chip = surface.tree().find_key("bound.chips");
    check(
        chip != nullptr && strings(property(*chip, "values")) ==
            std::vector<std::string>{"seed", "next"},
        "controlled ChipInput onChange did not round-trip through bound state"
    );
}

} // namespace

int strata_test_chip_input(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2) throw std::invalid_argument("expected resource root");
        const std::filesystem::path resource_root(arguments[1]);
        const auto bundle = load_bundle();
        test_uncontrolled_editor_lifecycle(resource_root, bundle);
        test_controlled_binding_round_trip(resource_root, bundle);
        std::cout << "strata_chip_input_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_chip_input_tests: " << error.what() << '\n';
        return 1;
    }
}
