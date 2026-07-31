#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/source.hpp"
#include "data/json.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "runtime/value.hpp"
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
            "unexpected form fixture import '" + std::string(path) + "'"
        );
    };
}

[[nodiscard]] const std::string* retained_string(
    const strata::ui::RetainedNode* node,
    const std::string_view name
) {
    const strata::runtime::Value* value = node != nullptr ? node->retained_value(name) : nullptr;
    return value != nullptr ? value->string() : nullptr;
}

[[nodiscard]] const bool* scalar_boolean(
    const strata::ui::RetainedNode* node,
    const std::string_view name
) {
    if (node == nullptr) return nullptr;
    const auto found = node->description().properties.find(name);
    return found != node->description().properties.end() && found->second.value() != nullptr
        ? found->second.value()->boolean()
        : nullptr;
}

[[nodiscard]] bool has_direct_text(
    const strata::ui::RetainedNode* node,
    const std::string_view expected
) {
    if (node == nullptr) return false;
    for (const std::unique_ptr<strata::ui::RetainedNode>& child : node->children()) {
        if (child->description().type != "Text") continue;
        const auto text = child->description().properties.find("text");
        if (text != child->description().properties.end() && text->second.value() != nullptr &&
            text->second.value()->string() != nullptr &&
            *text->second.value()->string() == expected) {
            return true;
        }
    }
    return false;
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

[[nodiscard]] bool contains_form_submit_request(
    const strata::data::JsonValue& value,
    const std::string_view expected_key
) {
    if (value.object() != nullptr) {
        const strata::data::JsonValue* kind = value.find("kind");
        const strata::data::JsonValue* key = value.find("key");
        if (kind != nullptr && kind->string() != nullptr &&
            *kind->string() == "form-submit-request" &&
            key != nullptr && key->string() != nullptr && *key->string() == expected_key) {
            return true;
        }
        for (const auto& [name, child] : *value.object()) {
            static_cast<void>(name);
            if (contains_form_submit_request(child, expected_key)) return true;
        }
    }
    if (value.array() != nullptr) {
        for (const strata::data::JsonValue& child : *value.array()) {
            if (contains_form_submit_request(child, expected_key)) return true;
        }
    }
    return false;
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

[[nodiscard]] strata::runtime::Value keyed_payload(const std::string_view key) {
    return strata::runtime::Value(
        std::vector<std::pair<std::string, strata::runtime::Value>>{
            {
                "key",
                strata::runtime::Value(strata::runtime::KeyValue{std::string(key)}),
            },
        }
    );
}

void test_visible_form_validation(
    const std::filesystem::path& registry_path,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component ValidationFixture() {
  state name = "xy";
  state amount = 9;
  Form(key: "validation.form") {
    Field(
      key: "validation.name-field",
      name: "name",
      label: "Name",
      inputKey: "validation.name",
      required: true,
      minLength: 3,
      minLengthMessage: "Use at least three characters.",
      help: "Public display name."
    ) {
      TextBox(key: "validation.name", bind: name)
    }
    Field(
      key: "validation.amount-field",
      name: "amount",
      label: "Amount",
      inputKey: "validation.amount",
      min: 3,
      max: 8,
      rangeMessage: "Choose an amount from 3 to 8."
    ) {
      NumberField(key: "validation.amount", bind: amount, min: 0, max: 10)
    }
    Field(
      key: "validation.readonly-field",
      name: "readonly",
      label: "Read only",
      inputKey: "validation.readonly",
      readOnly: true
    ) {
      Panel { TextBox(key: "validation.readonly", text: "fixed") }
    }
    Field(
      key: "validation.disabled-field",
      name: "disabled",
      label: "Disabled",
      inputKey: "validation.disabled",
      enabled: false,
      required: true
    ) {
      Panel { TextBox(key: "validation.disabled", text: "") }
    }
    Button(
      key: "validation.submit",
      label: "Validate",
      onClick: action("form.validate", key: "validation.form", focusFirstInvalid: true)
    )
  }
}
overlay Main { root ValidationFixture() }
)";

    strata::runtime::ApplicationContext application("form-validation", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"form-validation.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "form validation fixture did not activate");

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
        "form-validation",
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

    const strata::ui::RetainedNode* name_field = surface.tree().find_key(
        "validation.name-field"
    );
    check(
        has_direct_text(name_field, "Public display name."),
        "Field help was not projected into visible supporting content"
    );
    const strata::ui::RetainedNode* readonly = surface.tree().find_key("validation.readonly");
    const strata::ui::RetainedNode* disabled = surface.tree().find_key("validation.disabled");
    check(
        scalar_boolean(readonly, "readOnly") != nullptr &&
            *scalar_boolean(readonly, "readOnly"),
        "read-only Field capability did not reach a nested editor"
    );
    check(
        scalar_boolean(disabled, "enabled") != nullptr &&
            !*scalar_boolean(disabled, "enabled"),
        "disabled Field capability did not reach a nested editor"
    );
    static_cast<void>(surface.input().click("validation.disabled"));
    check(
        !surface.input().focused_key().has_value() ||
            *surface.input().focused_key() != "validation.disabled",
        "disabled Field descendant accepted focus"
    );
    static_cast<void>(surface.input().click("validation.readonly"));
    static_cast<void>(surface.input().text("changed"));
    check(
        surface.input().edited_text(readonly->identity()) != nullptr &&
            *surface.input().edited_text(readonly->identity()) == "fixed",
        "read-only Field descendant accepted text mutation"
    );

    static_cast<void>(surface.input().click("validation.amount"));
    static_cast<void>(surface.input().key("up"));
    static_cast<void>(surface.frame(1'500'000));
    const strata::ui::RetainedNode* amount_input = surface.tree().find_key("validation.amount");
    const std::optional<strata::ui::TextEditorSnapshot> amount_editor = amount_input != nullptr
        ? surface.input().editor_snapshot(amount_input->identity())
        : std::nullopt;
    check(
        amount_editor.has_value() && amount_editor->text == "10" && amount_editor->caret == 2U &&
            amount_editor->selection_start == 2U && amount_editor->selection_end == 2U,
        "NumberField ArrowUp did not apply its step and place the caret after the value"
    );

    const strata::ui::InputOperationResult validation = surface.input().click(
        "validation.submit"
    );
    check(
        !validation.action_outcomes.empty(),
        "form validation action did not produce an outcome"
    );
    static_cast<void>(surface.frame(2'000'000));

    name_field = surface.tree().find_key("validation.name-field");
    const strata::ui::RetainedNode* amount_field = surface.tree().find_key(
        "validation.amount-field"
    );
    const strata::ui::RetainedNode* disabled_field = surface.tree().find_key(
        "validation.disabled-field"
    );
    check(
        retained_string(name_field, "strata.form.error") != nullptr &&
            *retained_string(name_field, "strata.form.error") ==
                "Use at least three characters.",
        "authored minimum-length validation message was not retained"
    );
    check(
        retained_string(amount_field, "strata.form.error") != nullptr &&
            *retained_string(amount_field, "strata.form.error") ==
                "Choose an amount from 3 to 8.",
        "numeric Field range validation did not use the effective NumberField value"
    );
    check(
        retained_string(disabled_field, "strata.form.error") == nullptr,
        "disabled Field participated in Form validation"
    );
    check(
        has_direct_text(name_field, "Use at least three characters.") &&
            has_direct_text(amount_field, "Choose an amount from 3 to 8."),
        "retained validation errors were not projected as visible Field content"
    );
    check(
        surface.input().focused_key().has_value() &&
            *surface.input().focused_key() == "validation.name",
        "form validation did not focus the first invalid input"
    );

    static_cast<void>(surface.input().click("validation.name"));
    static_cast<void>(surface.input().key(
        "a",
        strata::ui::KeyModifiers{false, true, false, false}
    ));
    static_cast<void>(surface.input().text("Valid name"));
    static_cast<void>(surface.frame(3'000'000));

    name_field = surface.tree().find_key("validation.name-field");
    check(
        retained_string(name_field, "strata.form.error") == nullptr,
        "validated Field did not clear its error after a valid edit"
    );
    check(
        has_direct_text(name_field, "Public display name.") &&
            !has_direct_text(name_field, "Use at least three characters."),
        "Field did not restore help after its validation error cleared"
    );
}

void test_canonical_form_submission(
    const std::filesystem::path& registry_path,
    const std::shared_ptr<const strata::runtime::ApplicationBundle>& bundle
) {
    constexpr std::string_view source = R"(
component SubmissionFixture() {
  state controlled = "ready";
  Panel(key: "submission.root", layout: { kind: "COLUMN", width: { weight: 1 }, height: { weight: 1 }, gap: 12 }) {
    Form(key: "submission.valid") {
      Field(
        key: "submission.valid-field",
        name: "valid",
        label: "Valid",
        inputKey: "submission.valid-input",
        required: true
      ) {
        TextBox(key: "submission.valid-input", bind: controlled)
      }
      Button(
        key: "submission.button",
        label: "Submit",
        onClick: action("form.submit", key: "submission.valid")
      )
    }
    Form(key: "submission.invalid") {
      Field(
        key: "submission.invalid-field",
        name: "invalid",
        label: "Invalid",
        inputKey: "submission.invalid-input",
        required: true
      ) {
        TextBox(key: "submission.invalid-input", text: "")
      }
      Button(
        key: "submission.invalid-button",
        label: "Submit invalid",
        onClick: action("form.submit", key: "submission.invalid")
      )
    }
  }
}
overlay Main { root SubmissionFixture() }
)";

    strata::runtime::ApplicationContext application("form-submission", bundle);
    const strata::runtime::ActivationResult activation = application.compile_and_activate(
        strata::compiler::ModuleSource{"form-submission.strata", std::string(source)},
        no_imports(),
        0U
    );
    check(activation.activated(), "form submission fixture did not activate");

    std::size_t observations = 0U;
    std::vector<std::optional<std::string>> observation_sources;
    const std::shared_ptr<const strata::runtime::ActionContract> observation_contract =
        bundle->action_registry().contract("form-submit");
    check(observation_contract != nullptr, "form-submit observation contract is missing");
    auto observation_registration = application.actions().register_handler(
        observation_contract,
        "strata.tests.form-submission",
        [&observations, &observation_sources](const strata::runtime::ActionContext& context) {
            ++observations;
            observation_sources.push_back(context.event.source_key);
            return strata::runtime::ActionHandlerResult::handled;
        }
    );
    static_cast<void>(observation_registration);

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
        "form-submission",
        application,
        strata::runtime::LayerRole::overlay,
        "Main",
        environment,
        strata::ui::TextEngine::load_control_font(
            resources,
            strata::resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        )
    );
    static_cast<void>(surface.frame(10'000'000));

    const strata::ui::RetainedNode* valid_form = surface.tree().find_key("submission.valid");
    const strata::ui::RetainedNode* invalid_form = surface.tree().find_key("submission.invalid");
    check(valid_form != nullptr && invalid_form != nullptr, "submission Forms were not retained");
    const strata::data::JsonValue* semantics = surface.semantics().find(valid_form->identity());
    const strata::data::JsonValue* semantic_actions = semantics != nullptr
        ? semantics->find("actions")
        : nullptr;
    check(
        semantic_actions != nullptr && semantic_actions->array() != nullptr &&
            std::ranges::any_of(
                *semantic_actions->array(),
                [](const strata::data::JsonValue& action) {
                    return action.string() != nullptr && *action.string() == "activate";
                }
            ),
        "Form semantics did not expose activation"
    );

    const strata::ui::InjectedActionResult invalid = surface.input().dispatch_action(
        "form.submit",
        keyed_payload("submission.invalid"),
        "test-form-submit",
        std::optional<std::string>("submission.invalid"),
        strata::runtime::Value{}
    );
    check(
        invalid.outcome.status == strata::runtime::ActionDispatchStatus::handled &&
            outcomes_contain(invalid.input, "form.submit") && observations == 0U,
        "invalid direct Form submission was not handled without observation"
    );
    const strata::ui::RetainedNode* invalid_field = surface.tree().find_key(
        "submission.invalid-field"
    );
    check(
        retained_string(invalid_field, "strata.form.error") != nullptr,
        "invalid Form submission did not publish its visible validation error"
    );
    static_cast<void>(surface.frame(11'000'000));
    invalid_field = surface.tree().find_key("submission.invalid-field");
    check(
        has_direct_text(invalid_field, "This field is required."),
        "invalid Form submission did not expose its error on the next description frame"
    );
    const strata::ui::InputOperationResult invalid_button = surface.input().click(
        "submission.invalid-button"
    );
    check(
        outcomes_contain(invalid_button, "form.submit") && observations == 0U,
        "invalid submit Button bypassed Form validation or emitted its observation"
    );

    const strata::ui::InputOperationResult button = surface.input().click("submission.button");
    check(
        outcomes_contain(button, "form.submit") && outcomes_contain(button, "form-submit") &&
            observations == 1U,
        "submit Button did not pass through validation before observation"
    );
    check(
        std::ranges::any_of(button.events, [](const strata::data::JsonValue& event) {
            return contains_form_submit_request(event, "submission.valid");
        }),
        "submit Button did not preserve the exact Form key in its framework request"
    );

    static_cast<void>(surface.input().click("submission.valid-input"));
    const strata::ui::InputOperationResult enter = surface.input().key("enter");
    check(
        outcomes_contain(enter, "form.submit") && observations == 2U,
        "single-line Enter did not submit its retained ancestor Form"
    );

    const strata::ui::InjectedActionResult focus = surface.input().dispatch_action(
        "focus.request",
        keyed_payload("submission.valid"),
        "test-focus",
        std::optional<std::string>("submission.valid"),
        strata::runtime::Value{}
    );
    check(
        focus.outcome.status == strata::runtime::ActionDispatchStatus::handled,
        "Form could not receive semantic focus"
    );
    const strata::ui::InputOperationResult default_activation = surface.input().key("enter");
    check(
        outcomes_contain(default_activation, "form.submit") && observations == 3U &&
            std::ranges::any_of(default_activation.events, [](const strata::data::JsonValue& event) {
                return contains_form_submit_request(event, "submission.valid");
            }),
        "default Form keyboard activation did not resolve the retained Form key"
    );

    static_cast<void>(surface.input().enqueue(std::vector<strata::ui::SurfaceInputEvent>{
        strata::ui::NavigationInputEvent{"activate"},
    }));
    const strata::ui::SurfaceFrame semantic_activation = surface.frame(20'000'000);
    check(
        outcomes_contain(semantic_activation.lifecycle_input, "form.submit") && observations == 4U,
        "platform semantic activation did not use canonical Form submission"
    );

    const strata::ui::InjectedActionResult direct = surface.input().dispatch_action(
        "form.submit",
        keyed_payload("submission.valid"),
        "test-form-submit",
        std::optional<std::string>("submission.invalid"),
        strata::runtime::Value{}
    );
    check(
        direct.outcome.status == strata::runtime::ActionDispatchStatus::handled &&
            outcomes_contain(direct.input, "form.submit") && observations == 5U &&
            observation_sources.size() == 5U && observation_sources.back().has_value() &&
            *observation_sources.back() == "submission.valid",
        "direct declarative Form submission did not route or observe the payload Form key"
    );
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2) throw std::invalid_argument("expected registry path");
        const std::filesystem::path registry_path(arguments[1]);
        const auto bundle = load_bundle(registry_path);
        test_visible_form_validation(registry_path, bundle);
        test_canonical_form_submission(registry_path, bundle);
        std::cout << "strata_form_validation_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_form_validation_tests: " << error.what() << '\n';
        return 1;
    }
}
