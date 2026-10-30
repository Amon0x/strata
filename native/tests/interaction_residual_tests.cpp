#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/module.hpp"
#include "data/json.hpp"
#include "font/opentype.hpp"
#include "resource/resource.hpp"
#include "resource/svg_image.hpp"
#include "runtime/application.hpp"
#include "runtime/expression.hpp"
#include "runtime/formatting.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/command.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/notification.hpp"
#include "ui/semantics.hpp"
#include "ui/status.hpp"
#include "ui/surface.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/choice_model.hpp"
#include "ui/widget/color_picker_model.hpp"
#include "ui/widget/date_picker_model.hpp"
#include "ui/widget/editor_geometry.hpp"
#include "ui/widget/markdown_model.hpp"
#include "ui/widget/range_slider_model.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/shell_model.hpp"
#include "ui/widget/subtarget.hpp"
#include "ui/widget/zoom_view_model.hpp"

namespace {

using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

[[nodiscard]] runtime::Value
object(std::initializer_list<std::pair<std::string, runtime::Value>> fields) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>(fields));
}

[[nodiscard]] std::shared_ptr<const ui::DescriptionChildren>
children(std::vector<std::shared_ptr<const ui::DescriptionNode>> values) {
    return std::make_shared<const ui::EagerDescriptionChildren>(std::move(values));
}

[[nodiscard]] std::shared_ptr<const ui::DescriptionNode>
node(std::string type, std::optional<std::string> key,
     std::vector<std::shared_ptr<const ui::DescriptionNode>> nested = {},
     ui::DescriptionNode::Properties properties = {},
     std::vector<ui::DescriptionBehavior> behaviors = {}) {
    return ui::DescriptionNode::create(std::move(type), std::move(key), "/interaction-residual",
                                       "interaction residual fixture", std::move(properties),
                                       children(std::move(nested)), std::move(behaviors));
}

[[nodiscard]] runtime::ExpressionValue layout(const double width, const double height,
                                              std::string kind = "PANEL") {
    return runtime::ExpressionValue(object({
        {"height", runtime::Value(height)},
        {"kind", runtime::Value(std::move(kind))},
        {"width", runtime::Value(width)},
    }));
}

[[nodiscard]] ui::DescriptionNode::Properties sized(const double width, const double height,
                                                    std::string kind = "PANEL") {
    return {{"$layout", layout(width, height, std::move(kind))}};
}

[[nodiscard]] std::shared_ptr<const runtime::ApplicationBundle> load_bundle() {
    return runtime::ApplicationBundle::create();
}

[[nodiscard]] ui::Point center(const ui::Rect bounds) noexcept {
    return ui::Point{bounds.x + bounds.width * 0.5, bounds.y + bounds.height * 0.5};
}

[[nodiscard]] std::vector<std::string> semantic_actions(const data::JsonValue& semantics) {
    std::vector<std::string> result;
    const data::JsonValue* actions = semantics.find("actions");
    if (actions == nullptr || actions->array() == nullptr)
        return result;
    for (const data::JsonValue& action : *actions->array()) {
        if (action.string() != nullptr)
            result.push_back(*action.string());
    }
    return result;
}

[[nodiscard]] std::string semantic_value_text(const data::JsonValue& semantics) {
    const data::JsonValue* state = semantics.find("state");
    const data::JsonValue* value = state != nullptr ? state->find("valueText") : nullptr;
    return value != nullptr && value->string() != nullptr ? *value->string() : std::string{};
}

class InputFixture final {
  private:
    std::shared_ptr<const runtime::ApplicationBundle> bundle_;
    runtime::ApplicationContext application_;
    std::shared_ptr<const ui::TextEngine> text_;

  public:
    struct FrameResult final {
        ui::ReconcileStats reconciliation;
        ui::InputOperationResult input;
        ui::LayoutOperationCounters layout;
    };

    InputFixture(std::shared_ptr<const runtime::ApplicationBundle> bundle,
                 const std::filesystem::path& resources)
        : bundle_(std::move(bundle)), application_("interaction-residual", bundle_),
          text_(ui::TextEngine::load_default_fonts(resources)), commands_(widgets_),
          input_(
              "interaction-residual", "interaction-residual/host-owner", application_, widgets_,
              behaviors_, status_, notifications_, {},
              [this](const ui::RetainedNode*, const std::string_view) { ++invalidations_; }, {},
              [this](const ui::RetainedNode& owner, const ui::LayoutRecord& record,
                     const std::string_view value,
                     const ui::Point position) -> std::optional<std::size_t> {
                  const ui::TextLayout shaped = text_->layout(owner, value);
                  const ui::Point origin{record.content_bounds.x, record.content_bounds.y};
                  const std::size_t utf16 = ui::text_layout_hit_offset(
                      shaped, ui::Point{position.x - origin.x, position.y - origin.y});
                  return ui::utf8_byte_for_utf16_offset(value, utf16);
              },
              [this](const ui::RetainedNode& owner, const std::string_view value) {
                  return text_->shape(owner, value).metrics.width;
              },
              {}, {}, ui::InputProcessingConfig{}, nullptr,
              [this](const ui::RetainedNode& owner, const std::string_view value,
                     const ui::TextLayoutOptions& options) {
                  return text_->layout(owner, value, options);
              }) {
        ui::WidgetInputPhase probe;
        probe.focusable = true;
        widgets_.register_input_phase("Probe", std::move(probe));
    }

    void adopt(std::shared_ptr<const ui::DescriptionNode> description, const double scale = 1.0) {
        tree_.clear();
        static_cast<void>(tree_.reconcile(std::move(description)));
        static_cast<void>(input_.prepare(tree_));
        const ui::LayoutResult& result = layout_.layout(tree_, ui::LayoutEnvironment{
                                                                   0U,
                                                                   ui::Rect{0.0, 0.0, 640.0, 480.0},
                                                                   scale,
                                                                   {},
                                                                   ui::PointSnapPolicy::nearest,
                                                                   ui::RectangleSnapPolicy::outward,
                                                                   false,
                                                               });
        input_.publish_layout(result);
        commands_.rebuild(tree_);
        input_.publish_commands(commands_);
        tree_.clear_dirty();
    }

    [[nodiscard]] FrameResult frame(std::shared_ptr<const ui::DescriptionNode> description) {
        input_.begin_tree_update();
        ui::ReconcileStats reconciliation = tree_.reconcile(std::move(description));
        ui::InputOperationResult input = input_.prepare(tree_);
        const ui::LayoutResult& result = layout_.layout(tree_, ui::LayoutEnvironment{
                                                                   0U,
                                                                   ui::Rect{0.0, 0.0, 640.0, 480.0},
                                                                   1.0,
                                                                   {},
                                                                   ui::PointSnapPolicy::nearest,
                                                                   ui::RectangleSnapPolicy::outward,
                                                                   false,
                                                               });
        const ui::LayoutOperationCounters operations = result.operations;
        input_.publish_layout(result);
        commands_.rebuild(tree_);
        input_.publish_commands(commands_);
        static_cast<void>(input_.after_layout());
        tree_.clear_dirty();
        return FrameResult{
            std::move(reconciliation),
            std::move(input),
            operations,
        };
    }

    [[nodiscard]] ui::InputOperationResult pointer(std::vector<ui::PointerInputEvent> events) {
        std::vector<ui::SurfaceInputEvent> queued;
        queued.reserve(events.size());
        for (ui::PointerInputEvent& event : events)
            queued.emplace_back(std::move(event));
        static_cast<void>(input_.enqueue(std::move(queued)));
        return input_.process_queued();
    }

    [[nodiscard]] const ui::LayoutRecord& bounds(const std::string_view key) const {
        const ui::RetainedNode* retained = tree_.find_key(key);
        const ui::LayoutRecord* record =
            retained != nullptr ? layout_.result().find(retained->identity()) : nullptr;
        if (record == nullptr)
            throw std::runtime_error("missing fixture layout record");
        return *record;
    }

    [[nodiscard]] ui::TextLayout text_layout(const std::string_view key) const {
        const ui::RetainedNode* retained = tree_.find_key(key);
        if (retained == nullptr)
            throw std::runtime_error("missing text-layout fixture node");
        const auto property = retained->description().properties.find("text");
        const runtime::Value* value = property != retained->description().properties.end()
                                          ? property->second.value()
                                          : nullptr;
        if (value == nullptr || value->string() == nullptr) {
            throw std::runtime_error("text-layout fixture node has no text");
        }
        return text_->layout(*retained, *value->string());
    }

    [[nodiscard]] ui::TextLayout text_layout(const std::string_view key, const std::string_view value,
                                             const ui::TextLayoutOptions& options) const {
        const ui::RetainedNode* retained = tree_.find_key(key);
        if (retained == nullptr)
            throw std::runtime_error("missing text-layout fixture node");
        return text_->layout(*retained, value, options);
    }

    [[nodiscard]] bool focused(const std::string_view key) const {
        const ui::RetainedNode* retained = tree_.find_key(key);
        return retained != nullptr && input_.focused(retained->identity());
    }

    [[nodiscard]] std::shared_ptr<const runtime::ActionValue>
    notification_action(std::string message) const {
        const std::shared_ptr<const runtime::ActionContract> contract =
            bundle_->action_registry().contract("notification.raise");
        if (contract == nullptr)
            throw std::runtime_error("notification action is unavailable");
        return std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
            std::make_shared<const runtime::Action>(
                contract, object({{"message", runtime::Value(std::move(message))}})),
            std::nullopt,
            {},
        });
    }

    [[nodiscard]] std::shared_ptr<const runtime::ActionValue>
    framework_action(const std::string_view id, runtime::Value payload) const {
        const std::shared_ptr<const runtime::ActionContract> contract =
            bundle_->action_registry().contract(id);
        if (contract == nullptr)
            throw std::runtime_error("framework action is unavailable");
        return std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
            std::make_shared<const runtime::Action>(contract, std::move(payload)),
            std::nullopt,
            {},
        });
    }

    ui::WidgetRegistry widgets_;
    ui::BehaviorRegistry behaviors_;
    ui::StatusFeedbackService status_;
    ui::NotificationService notifications_;
    ui::RetainedTree tree_;
    ui::LayoutEngine layout_;
    ui::CommandIndex commands_;
    ui::InputRouter input_;
    std::size_t invalidations_ = 0U;
};

[[nodiscard]] runtime::ExpressionValue
rich_span(std::string text, std::shared_ptr<const runtime::ActionValue> action) {
    std::vector<std::pair<std::string, runtime::ExpressionValue>> fields;
    fields.emplace_back("text", runtime::ExpressionValue(runtime::Value(std::move(text))));
    fields.emplace_back("action", runtime::ExpressionValue(std::move(action)));
    return runtime::ExpressionValue(
        std::make_shared<const runtime::ExpressionObjectValue>(std::move(fields)));
}

void test_primary_pointer_focus_default(InputFixture& fixture) {
    fixture.adopt(node(
        "Panel", "focus.root",
        {
            node("Probe", "focus.control", {}, sized(80.0, 32.0)),
            node("Panel", "focus.interactive-background", {}, sized(120.0, 40.0),
                 {ui::DescriptionBehavior{"strata.hoverable", true, runtime::Value{}, nullptr}}),
        },
        sized(300.0, 160.0, "COLUMN")));

    std::int32_t pointer_id = 100;
    const auto focus_control = [&fixture, &pointer_id] {
        const ui::Point point = center(fixture.bounds("focus.control").bounds);
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{point, ui::PointerEventType::press, pointer_id, 0},
            ui::PointerInputEvent{point, ui::PointerEventType::release, pointer_id++, 0},
        }));
        check(fixture.input_.focused_key().has_value() &&
                  *fixture.input_.focused_key() == "focus.control",
              "primary press did not focus its focusable target");
    };
    const ui::Point empty_background{280.0, 140.0};

    focus_control();
    const ui::RetainedNode* control = fixture.tree_.find_key("focus.control");
    check(control != nullptr && !fixture.input_.focus_visible(control->identity()),
          "pointer focus incorrectly retained a keyboard focus indicator");
    static_cast<void>(fixture.input_.key("alt", ui::KeyModifiers{.alt = true}));
    static_cast<void>(fixture.input_.key("tab", ui::KeyModifiers{.alt = true}));
    check(fixture.input_.focused_key() == "focus.control" &&
              !fixture.input_.focus_visible(control->identity()),
          "Alt-Tab changed focus or exposed a keyboard navigation indicator");
    static_cast<void>(fixture.input_.key("tab"));
    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "focus.control" &&
              fixture.input_.focus_visible(control->identity()),
          "keyboard traversal did not reveal focus on the already-focused control");
    focus_control();
    check(!fixture.input_.focus_visible(control->identity()),
          "pressing the already-focused control did not suppress its keyboard indicator");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{
            empty_background,
            ui::PointerEventType::press,
            pointer_id,
            1,
        },
        ui::PointerInputEvent{
            empty_background,
            ui::PointerEventType::release,
            pointer_id++,
            1,
        },
    }));
    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "focus.control",
          "secondary background press unexpectedly cleared focus");

    fixture.invalidations_ = 0U;
    const ui::InputOperationResult empty_press = fixture.pointer({
        ui::PointerInputEvent{
            empty_background,
            ui::PointerEventType::press,
            pointer_id,
            0,
        },
        ui::PointerInputEvent{
            empty_background,
            ui::PointerEventType::release,
            pointer_id++,
            0,
        },
    });
    check(!fixture.input_.focused_key().has_value() && !empty_press.events.empty(),
          "primary empty-background press did not blur the focused control");
    check(fixture.invalidations_ == 0U, "background blur escaped the retained input-dirty path");

    focus_control();
    const ui::Point interactive_background =
        center(fixture.bounds("focus.interactive-background").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{
            interactive_background,
            ui::PointerEventType::press,
            pointer_id,
            0,
        },
        ui::PointerInputEvent{
            interactive_background,
            ui::PointerEventType::release,
            pointer_id++,
            0,
        },
    }));
    check(!fixture.input_.focused_key().has_value(),
          "non-focusable interactive background retained stale focus");

    focus_control();
    ui::InputOperationResult containment;
    check(fixture.input_.set_focus_containment("focus.root", containment),
          "focus fixture could not establish containment");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{
            empty_background,
            ui::PointerEventType::press,
            pointer_id,
            0,
        },
        ui::PointerInputEvent{
            empty_background,
            ui::PointerEventType::release,
            pointer_id++,
            0,
        },
    }));
    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "focus.control",
          "contained background press escaped the active focus scope");
    static_cast<void>(fixture.input_.set_focus_containment(std::nullopt, containment));
}

void test_detached_portal_hit_testing(InputFixture& fixture) {
    ui::DescriptionNode::Properties portal{
        {"$layout", runtime::ExpressionValue(object({
                        {"anchorGap", runtime::Value(0.0)},
                        {"anchorPoint", object({
                                            {"x", runtime::Value(200.0)},
                                            {"y", runtime::Value(100.0)},
                                        })},
                        {"anchorSide", runtime::Value("BOTTOM")},
                        {"detachFromParentClip", runtime::Value(true)},
                        {"height", runtime::Value(30.0)},
                        {"kind", runtime::Value("PORTAL")},
                        {"width", runtime::Value(60.0)},
                    }))},
    };
    fixture.adopt(node("Panel", "portal.root",
                       {
                           node("Panel", "portal.clipped",
                                {node("Probe", "portal.control", {}, std::move(portal))},
                                {
                                    {"$layout", runtime::ExpressionValue(object({
                                                    {"clip", runtime::Value(true)},
                                                    {"height", runtime::Value(50.0)},
                                                    {"kind", runtime::Value("PANEL")},
                                                    {"width", runtime::Value(100.0)},
                                                }))},
                                }),
                       },
                       sized(640.0, 480.0)));
    const ui::Rect bounds = fixture.bounds("portal.control").bounds;
    check(bounds.x >= 200.0 && bounds.y >= 100.0,
          "detached portal fixture did not leave its clipped parent");
    const ui::Point position = center(bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{position, ui::PointerEventType::press, 1, 0},
        ui::PointerInputEvent{position, ui::PointerEventType::release, 1, 0},
    }));
    check(fixture.focused("portal.control"),
          "detached portal was visible outside its parent clip but could not be hit");
}

std::vector<ui::WidgetDismissal> lifecycle_dismissals;

/**
 * Widget types the framework has never heard of get modality, popup dismissal and projected
 * subtargets from their lifecycle alone: generic input code holds no widget names.
 */
void test_lifecycle_input_hooks(InputFixture& fixture) {
    ui::WidgetInputPhase trap;
    trap.focusable = true;
    trap.modal_open = [](const ui::RetainedNode& node) {
        const auto open = node.description().properties.find("open");
        const runtime::Value* value =
            open != node.description().properties.end() ? open->second.value() : nullptr;
        return value != nullptr && value->boolean() != nullptr && *value->boolean();
    };
    trap.modal_priority = 1;
    fixture.widgets_.register_input_phase("LifecycleTrap", std::move(trap));

    ui::WidgetInputPhase popup;
    popup.popup_retained = "$open";
    popup.popup_initial = "defaultOpen";
    popup.on_dismiss = [](ui::WidgetInputScope&, const ui::WidgetDismissal reason) {
        lifecycle_dismissals.push_back(reason);
        return false;
    };
    fixture.widgets_.register_input_phase("LifecyclePopup", std::move(popup));

    ui::WidgetInputPhase regions;
    regions.focusable = true;
    fixture.widgets_.register_input_phase("LifecycleRegions", std::move(regions));
    fixture.widgets_.register_subtargets(
        "LifecycleRegions",
        [](const ui::WidgetSubtargetContext& context) {
            const ui::Rect bounds = context.record.bounds;
            return std::vector<ui::WidgetSubtarget>{ui::WidgetSubtarget{
                context.node.identity(), "$flyout", ui::WidgetSubtargetKind::control, 0U,
                ui::Rect{bounds.right() + 100.0, bounds.y, 60.0, 40.0}, runtime::Value{}, {}, {},
                true, true, ui::detached_overlay_menu_z,
            }};
        },
        true);
    check(std::ranges::find(fixture.widgets_.detached_subtarget_types(), "LifecycleRegions") !=
              fixture.widgets_.detached_subtarget_types().end(),
          "a registered detached projection was not listed for detached hit testing");

    const auto trapped = [](const bool open) {
        return node("Panel", "lifecycle.root",
                    {
                        node("Probe", "lifecycle.outside", {}, sized(80.0, 32.0)),
                        node("LifecycleTrap", "lifecycle.trap",
                             {node("Probe", "lifecycle.inside", {}, sized(80.0, 32.0))},
                             {{"open", runtime::ExpressionValue(runtime::Value(open))},
                              {"$layout", layout(120.0, 60.0)}}),
                    },
                    sized(640.0, 480.0, "ROW"));
    };
    fixture.adopt(trapped(true));
    const ui::Point outside = center(fixture.bounds("lifecycle.outside").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{outside, ui::PointerEventType::press, 40, 0},
        ui::PointerInputEvent{outside, ui::PointerEventType::release, 40, 0},
    }));
    check(!fixture.focused("lifecycle.outside"),
          "an open lifecycle modal did not trap a press outside it");
    fixture.adopt(trapped(false));
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{outside, ui::PointerEventType::press, 41, 0},
        ui::PointerInputEvent{outside, ui::PointerEventType::release, 41, 0},
    }));
    check(fixture.focused("lifecycle.outside"), "a closed lifecycle modal still trapped input");

    lifecycle_dismissals.clear();
    const auto popup_tree = [] {
        return node("Panel", "lifecycle.popup-root",
                    {
                        node("LifecyclePopup", "lifecycle.popup",
                             {node("Probe", "lifecycle.popup-child", {}, sized(60.0, 30.0))},
                             {{"defaultOpen", runtime::ExpressionValue(runtime::Value(true))},
                              {"$layout", layout(100.0, 50.0)}}),
                        node("LifecycleRegions", "lifecycle.regions", {}, sized(80.0, 40.0)),
                    },
                    sized(640.0, 480.0, "ROW"));
    };
    fixture.adopt(popup_tree());
    const ui::Point child = center(fixture.bounds("lifecycle.popup-child").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{child, ui::PointerEventType::press, 42, 0},
        ui::PointerInputEvent{child, ui::PointerEventType::release, 42, 0},
    }));
    check(lifecycle_dismissals.empty(), "a press inside a lifecycle popup dismissed it");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{ui::Point{600.0, 450.0}, ui::PointerEventType::press, 43, 0},
        ui::PointerInputEvent{ui::Point{600.0, 450.0}, ui::PointerEventType::release, 43, 0},
    }));
    const runtime::Value* open = fixture.tree_.find_key("lifecycle.popup")->retained_value("$open");
    check(open != nullptr && open->boolean() != nullptr && !*open->boolean() &&
              lifecycle_dismissals == std::vector{ui::WidgetDismissal::outside_press},
          "an outside press did not close a lifecycle popup through its dismissal hook");

    fixture.adopt(popup_tree());
    lifecycle_dismissals.clear();
    static_cast<void>(fixture.input_.key("escape"));
    check(lifecycle_dismissals == std::vector{ui::WidgetDismissal::escape},
          "Escape did not dismiss a lifecycle popup through its dismissal hook");

    const ui::RetainedNode* owner = fixture.tree_.find_key("lifecycle.regions");
    const std::vector<ui::WidgetSubtarget> projected = fixture.input_.subtargets(owner->identity());
    check(projected.size() == 1U && projected.front().id == "$flyout",
          "a registered subtarget hook was not the node's projection");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{center(projected.front().bounds), ui::PointerEventType::press, 44, 0},
        ui::PointerInputEvent{center(projected.front().bounds), ui::PointerEventType::release, 44,
                              0},
    }));
    check(fixture.focused("lifecycle.regions"),
          "a detached lifecycle subtarget outside its owner's bounds was not hit");
}

void test_authored_popup_dismissal(InputFixture& fixture) {

    const auto description =
        node("Panel", "popup.fixture",
             {
                 node("Probe", "popup.anchor", {}, sized(80.0, 30.0)),
                 node("Popup", "popup.surface",
                      {
                          node("Probe", "popup.child", {}, sized(70.0, 40.0)),
                      },
                      {
                          {"defaultOpen", runtime::ExpressionValue(runtime::Value(true))},
                          {"$layout", runtime::ExpressionValue(object({
                                          {"anchorGap", runtime::Value(5.0)},
                                          {"anchorSide", runtime::Value("RIGHT")},
                                          {"anchorTarget", runtime::Value("popup.anchor")},
                                          {"height", runtime::Value(60.0)},
                                          {"kind", runtime::Value("PORTAL")},
                                          {"width", runtime::Value(90.0)},
                                      }))},
                      }),
                 node("Probe", "popup.outside", {},
                      {{"$layout", runtime::ExpressionValue(object({
                                       {"height", runtime::Value(30.0)},
                                       {"width", runtime::Value(80.0)},
                                   }))}}),
             },
             sized(640.0, 480.0, "ROW"));
    fixture.adopt(description);

    const ui::Point child = center(fixture.bounds("popup.child").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{child, ui::PointerEventType::press, 1U, 0},
        ui::PointerInputEvent{child, ui::PointerEventType::release, 1U, 0},
    }));
    const runtime::Value* open =
        fixture.tree_.find_key("popup.surface")->retained_value("$expanded");
    check(open == nullptr, "inside popup interaction dismissed the popup");

    const ui::Point anchor = center(fixture.bounds("popup.anchor").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{anchor, ui::PointerEventType::press, 2U, 0},
        ui::PointerInputEvent{anchor, ui::PointerEventType::release, 2U, 0},
    }));
    open = fixture.tree_.find_key("popup.surface")->retained_value("$expanded");
    check(open == nullptr, "anchor interaction dismissed its popup before activation");

    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{ui::Point{500.0, 400.0}, ui::PointerEventType::press, 3U, 0},
        ui::PointerInputEvent{ui::Point{500.0, 400.0}, ui::PointerEventType::release, 3U, 0},
    }));
    open = fixture.tree_.find_key("popup.surface")->retained_value("$expanded");
    check(open != nullptr && open->boolean() != nullptr && !*open->boolean(),
          "outside interaction did not close the popup");
}

void test_rich_text_press_arm(InputFixture& fixture) {
    const std::shared_ptr<const runtime::ActionValue> first =
        fixture.notification_action("first link");
    const std::shared_ptr<const runtime::ActionValue> second =
        fixture.notification_action("second link");
    std::vector<runtime::ExpressionValue> expressed;
    expressed.push_back(rich_span("first", first));
    expressed.push_back(rich_span(" second", second));
    ui::DescriptionNode::Properties rich = sized(300.0, 32.0);
    rich.emplace("text", runtime::ExpressionValue(runtime::Value("first second")));
    rich.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
    rich.emplace("spans", runtime::ExpressionValue(
                              std::make_shared<const runtime::ExpressionListValue>(
                                  std::move(expressed))));
    fixture.adopt(node("Panel", "rich.root",
                       {
                           node("RichText", "rich.text", {}, std::move(rich)),
                       },
                       sized(400.0, 80.0, "COLUMN")));
    ui::RetainedNode* retained = fixture.tree_.find_key("rich.text");
    check(retained != nullptr, "RichText fixture was not retained");
    check(!fixture.input_.editor_snapshot(retained->identity()).has_value(),
          "selectable RichText leaked into the writable editor inspection projection");
    const std::vector<ui::WidgetSubtarget> links = fixture.input_.subtargets(retained->identity());
    const auto first_target =
        std::ranges::find(links, std::string("$link/0"), &ui::WidgetSubtarget::id);
    const auto second_target =
        std::ranges::find(links, std::string("$link/1"), &ui::WidgetSubtarget::id);
    check(first_target != links.end() && second_target != links.end() &&
              first_target->kind == ui::WidgetSubtargetKind::link &&
              second_target->kind == ui::WidgetSubtargetKind::link,
          "RichText link spans did not project first-class hit subtargets");

    const ui::Point first_point{
        first_target->bounds.right() - 0.5,
        first_target->bounds.y + first_target->bounds.height * 0.5,
    };
    const ui::Point second_point{
        second_target->bounds.x + 0.5,
        second_target->bounds.y + second_target->bounds.height * 0.5,
    };
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{first_point, ui::PointerEventType::press, 1, 0},
        ui::PointerInputEvent{second_point, ui::PointerEventType::release, 1, 0},
    }));
    check(fixture.notifications_.size() == 0U,
          "RichText activated the release link instead of the exact press-armed link");

    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{first_point, ui::PointerEventType::press, 2, 0},
        ui::PointerInputEvent{
            ui::Point{first_point.x + 8.0, first_point.y},
            ui::PointerEventType::move,
            2,
            0,
        },
        ui::PointerInputEvent{first_point, ui::PointerEventType::release, 2, 0},
    }));
    check(fixture.notifications_.size() == 0U,
          "RichText activated after the shared selection/drag slop cancelled its press");
    const std::optional<ui::StaticTextSelectionSnapshot> selection =
        fixture.input_.static_text_selection_snapshot(retained->identity());
    check(selection.has_value() && selection->selection_start != selection->selection_end,
          "RichText moved-link cancellation stopped participating in static selection");

    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{second_point, ui::PointerEventType::press, 3, 0},
        ui::PointerInputEvent{second_point, ui::PointerEventType::release, 3, 0},
    }));
    check(fixture.notifications_.size() == 1U &&
              fixture.notifications_.snapshot().visible.front().request.message == "second link",
          "matching RichText press/release did not activate its span-local action");
}

void test_static_text_state_partition(InputFixture& fixture) {
    const std::shared_ptr<const runtime::ActionValue> action =
        fixture.notification_action("link only");
    std::vector<runtime::ExpressionValue> expressed;
    expressed.push_back(rich_span("link only", action));

    ui::DescriptionNode::Properties ordinary = sized(240.0, 28.0);
    ordinary.emplace("text", runtime::ExpressionValue(runtime::Value("ordinary title")));
    ordinary.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
    ui::DescriptionNode::Properties opted_out = sized(240.0, 28.0);
    opted_out.emplace("text", runtime::ExpressionValue(runtime::Value("decorative title")));
    opted_out.emplace("selectable", runtime::ExpressionValue(runtime::Value(false)));
    opted_out.emplace("selectionContainer", runtime::ExpressionValue(runtime::Value("document")));
    ui::DescriptionNode::Properties linked = sized(240.0, 28.0);
    linked.emplace("text", runtime::ExpressionValue(runtime::Value("link only")));
    linked.emplace("selectable", runtime::ExpressionValue(runtime::Value(false)));
    linked.emplace("spans", runtime::ExpressionValue(
                                std::make_shared<const runtime::ExpressionListValue>(
                                    std::move(expressed))));
    ui::DescriptionNode::Properties button = sized(240.0, 28.0);
    button.emplace("label", runtime::ExpressionValue(runtime::Value("Clear selection")));
    fixture.adopt(node("Panel", "static.root",
                       {
                           node("Text", "static.ordinary", {}, std::move(ordinary)),
                           node("Text", "static.opted-out", {}, std::move(opted_out)),
                           node("RichText", "static.linked", {}, std::move(linked)),
                           node("Button", "static.button", {}, std::move(button)),
                       },
                       sized(300.0, 150.0, "COLUMN")));

    for (const std::string_view key : {
             std::string_view("static.ordinary"),
             std::string_view("static.opted-out"),
             std::string_view("static.linked"),
         }) {
        const ui::RetainedNode* retained = fixture.tree_.find_key(key);
        check(retained != nullptr, "static-text partition fixture lost a retained node");
        check(!fixture.input_.editor_snapshot(retained->identity()).has_value(),
              "read-only static/rich text appeared in the writable editors projection");
        check(!fixture.input_.static_text_selection_snapshot(retained->identity()).has_value(),
              "static text allocated read-only selection state before interaction");
    }

    const ui::Point ordinary_point = center(fixture.bounds("static.ordinary").bounds);
    const auto select_ordinary = [&fixture, ordinary_point](const std::int32_t pointer_id) {
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{
                ordinary_point,
                ui::PointerEventType::press,
                pointer_id,
                0,
            },
            ui::PointerInputEvent{
                ordinary_point,
                ui::PointerEventType::release,
                pointer_id,
                0,
            },
        }));
    };
    select_ordinary(11);
    const ui::RetainedNode* ordinary_node = fixture.tree_.find_key("static.ordinary");
    static_cast<void>(fixture.input_.text("must not mutate"));
    static_cast<void>(fixture.input_.ime_preedit("composition", 0U, 3U));
    const std::optional<ui::StaticTextSelectionSnapshot> ordinary_selection =
        fixture.input_.static_text_selection_snapshot(ordinary_node->identity());
    check(ordinary_selection.has_value() && ordinary_selection->text == "ordinary title" &&
              !fixture.input_.editor_snapshot(ordinary_node->identity()).has_value(),
          "static selection accepted text/IME input or re-entered writable editor state");

    const ui::Point opted_out_point = center(fixture.bounds("static.opted-out").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{opted_out_point, ui::PointerEventType::press, 13, 0},
        ui::PointerInputEvent{opted_out_point, ui::PointerEventType::release, 13, 0},
    }));
    const ui::RetainedNode* opted_out_node = fixture.tree_.find_key("static.opted-out");
    check(opted_out_node != nullptr && !fixture.input_.focused_key().has_value() &&
              !fixture.input_.static_text_selection_snapshot(opted_out_node->identity())
                   .has_value() &&
              !fixture.input_.static_text_selection_snapshot(ordinary_node->identity()).has_value(),
          "nonselectable Text did not blur or overrode its selection opt-out");

    select_ordinary(14);
    const ui::Point button_point = center(fixture.bounds("static.button").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{button_point, ui::PointerEventType::press, 15, 0},
        ui::PointerInputEvent{button_point, ui::PointerEventType::release, 15, 0},
    }));
    check(!fixture.input_.static_text_selection_snapshot(ordinary_node->identity()).has_value(),
          "Button press did not clear static selection ownership");

    select_ordinary(16);
    const ui::RetainedNode* linked_node = fixture.tree_.find_key("static.linked");
    check(linked_node != nullptr, "link-only RichText was not retained");
    const std::vector<ui::WidgetSubtarget> links =
        fixture.input_.subtargets(linked_node->identity());
    check(!links.empty(), "link-only RichText lost its independent link subtarget");
    const ui::Point link_point = center(links.front().bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{link_point, ui::PointerEventType::press, 12, 0},
        ui::PointerInputEvent{link_point, ui::PointerEventType::release, 12, 0},
    }));
    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "static.linked" &&
              fixture.notifications_.size() > 0U &&
              fixture.notifications_.snapshot().visible.back().request.message == "link only",
          "link-only RichText no longer focused and activated independently of selection state");
    check(!fixture.input_.static_text_selection_snapshot(linked_node->identity()).has_value() &&
              !fixture.input_.static_text_selection_snapshot(ordinary_node->identity()).has_value(),
          "link-only press retained the old owner or allocated opt-out selection state");

    select_ordinary(17);
    const ui::Point background_point{290.0, 140.0};
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{background_point, ui::PointerEventType::press, 18, 0},
        ui::PointerInputEvent{background_point, ui::PointerEventType::release, 18, 0},
    }));
    check(!fixture.input_.static_text_selection_snapshot(ordinary_node->identity()).has_value(),
          "background press did not clear static selection ownership");
}

void test_wrapped_static_text_navigation(InputFixture& fixture) {
    constexpr std::string_view key = "static.wrapped";
    constexpr std::string_view content =
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda";
    const auto adopt = [&fixture, content, key] {
        ui::DescriptionNode::Properties text = sized(72.0, 180.0);
        text.emplace("text", runtime::ExpressionValue(runtime::Value(std::string(content))));
        text.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
        text.emplace("wrapWidth", runtime::ExpressionValue(runtime::Value(72.0)));
        text.emplace("wrapMode", runtime::ExpressionValue(runtime::Value("WORD")));
        fixture.adopt(node("Panel", "static.wrap-root",
                           {
                               node("Text", std::string(key), {}, std::move(text)),
                           },
                           sized(100.0, 200.0, "COLUMN")));
    };
    const auto point_on_line = [&fixture, key](const ui::TextLayout& layout, const std::size_t line,
                                               const double x) {
        const ui::LayoutRecord& record = fixture.bounds(key);
        return ui::Point{
            record.content_bounds.x + x,
            record.content_bounds.y + layout.lines[line].y + layout.lines[line].height * 0.5,
        };
    };
    const auto click = [&fixture](const ui::Point point, const std::int32_t pointer_id) {
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{point, ui::PointerEventType::press, pointer_id, 0},
            ui::PointerInputEvent{point, ui::PointerEventType::release, pointer_id, 0},
        }));
    };
    const auto selection = [&fixture, key]() {
        const ui::RetainedNode* retained = fixture.tree_.find_key(key);
        check(retained != nullptr, "wrapped static Text was not retained");
        const std::optional<ui::StaticTextSelectionSnapshot> result =
            fixture.input_.static_text_selection_snapshot(retained->identity());
        check(result.has_value(), "wrapped static Text did not retain a selection caret");
        return *result;
    };

    adopt();
    ui::TextLayout layout = fixture.text_layout(key);
    check(layout.lines.size() >= 4U, "wrapped static fixture did not produce visual lines");
    constexpr std::size_t home_line = 1U;
    const double home_x = layout.lines[home_line].x + layout.lines[home_line].width * 0.6;
    click(point_on_line(layout, home_line, home_x), 31);
    static_cast<void>(fixture.input_.key("home"));
    check(ui::utf16_offset_for_utf8_byte(content, selection().caret) ==
              layout.lines[home_line].text_start_offset,
          "Home used a hard newline instead of the current wrapped visual line");
    static_cast<void>(fixture.input_.key("end"));
    check(ui::utf16_offset_for_utf8_byte(content, selection().caret) ==
              layout.lines[home_line].text_end_offset,
          "End used a hard newline instead of the current wrapped visual line");

    adopt();
    layout = fixture.text_layout(key);
    constexpr std::size_t vertical_line = 1U;
    const double requested_x =
        layout.lines[vertical_line].x + layout.lines[vertical_line].width * 0.7;
    const std::size_t initial_utf16 =
        ui::text_layout_line_offset_at_x(layout, vertical_line, requested_x);
    const double x_goal = ui::shaped_caret_x(layout.shaped, vertical_line, initial_utf16);
    click(point_on_line(layout, vertical_line, requested_x), 32);
    static_cast<void>(fixture.input_.key("down"));
    check(ui::utf16_offset_for_utf8_byte(content, selection().caret) ==
              ui::text_layout_line_offset_at_x(layout, vertical_line + 1U, x_goal),
          "first Down did not preserve the immutable-layout caret x goal");
    static_cast<void>(fixture.input_.key("down"));
    check(ui::utf16_offset_for_utf8_byte(content, selection().caret) ==
              ui::text_layout_line_offset_at_x(layout, vertical_line + 2U, x_goal),
          "repeated Down recomputed instead of preserving the caret x goal");
    static_cast<void>(fixture.input_.key("up"));
    check(ui::utf16_offset_for_utf8_byte(content, selection().caret) ==
              ui::text_layout_line_offset_at_x(layout, vertical_line + 1U, x_goal),
          "Up did not return along the preserved caret x goal");

    adopt();
    layout = fixture.text_layout(key);
    const std::size_t shift_anchor_utf16 =
        ui::text_layout_line_offset_at_x(layout, vertical_line, requested_x);
    const double shift_x_goal =
        ui::shaped_caret_x(layout.shaped, vertical_line, shift_anchor_utf16);
    click(point_on_line(layout, vertical_line, requested_x), 33);
    static_cast<void>(fixture.input_.key("down", ui::KeyModifiers{true, false, false, false}));
    const ui::StaticTextSelectionSnapshot shifted = selection();
    check(ui::utf16_offset_for_utf8_byte(content, shifted.selection_start) == shift_anchor_utf16 &&
              ui::utf16_offset_for_utf8_byte(content, shifted.selection_end) ==
                  ui::text_layout_line_offset_at_x(layout, vertical_line + 1U, shift_x_goal),
          "Shift+Down lost its anchor or visual-line focus offset");
}

void test_wrapped_editor_pointer_navigation(InputFixture& fixture) {
    constexpr std::string_view key = "editor.wrapped";
    constexpr std::string_view content =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const auto adopt = [&fixture, content, key] {
        ui::DescriptionNode::Properties editor = sized(72.0, 180.0);
        editor.emplace("text", runtime::ExpressionValue(runtime::Value(std::string(content))));
        editor.emplace("wrapWidth", runtime::ExpressionValue(runtime::Value(72.0)));
        editor.emplace("wrapMode", runtime::ExpressionValue(runtime::Value("CHARACTER")));
        fixture.adopt(node("Panel", "editor.wrap-root",
                           {
                               node("TextArea", std::string(key), {}, std::move(editor)),
                           },
                           sized(100.0, 200.0, "COLUMN")),
                      1.5);
    };
    const auto first_cluster_point = [&fixture, key](const ui::TextLayout& layout,
                                                     const std::size_t line) {
        const auto cluster = std::ranges::find_if(
            layout.shaped.clusters, [line](const font::ShapedCluster& candidate) {
                return candidate.line_index == line && !candidate.soft_wrap_gap;
            });
        check(cluster != layout.shaped.clusters.end(),
              "wrapped editor line lost its first cluster");
        const ui::LayoutRecord& record = fixture.bounds(key);
        return ui::Point{
            record.content_bounds.x + cluster->x + cluster->advance * 0.25,
            record.content_bounds.y + layout.lines[line].y + layout.lines[line].height * 0.5,
        };
    };
    const auto click = [&fixture](const ui::Point point, const std::int32_t pointer_id) {
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{point, ui::PointerEventType::press, pointer_id, 0},
            ui::PointerInputEvent{point, ui::PointerEventType::release, pointer_id, 0},
        }));
    };
    const auto snapshot = [&fixture, key] {
        const ui::RetainedNode* retained = fixture.tree_.find_key(key);
        check(retained != nullptr, "wrapped TextArea was not retained");
        const std::optional<ui::TextEditorSnapshot> result =
            fixture.input_.editor_snapshot(retained->identity());
        check(result.has_value(), "wrapped TextArea lost writable editor state");
        return *result;
    };

    constexpr std::size_t clicked_line = 1U;
    adopt();
    ui::TextLayout layout = fixture.text_layout(key);
    check(layout.lines.size() >= 4U, "wrapped TextArea fixture did not produce visual lines");
    const ui::Point point = first_cluster_point(layout, clicked_line);
    click(point, 51);
    check(ui::utf16_offset_for_utf8_byte(content, snapshot().caret) ==
              layout.lines[clicked_line].text_start_offset,
          "wrapped TextArea click did not land at the requested first cluster");
    static_cast<void>(fixture.input_.key("home"));
    check(ui::utf16_offset_for_utf8_byte(content, snapshot().caret) ==
              layout.lines[clicked_line].text_start_offset,
          "editor Home lost pointer-selected affinity at a shared wrap offset");
    static_cast<void>(fixture.input_.key("end"));
    check(ui::utf16_offset_for_utf8_byte(content, snapshot().caret) ==
              layout.lines[clicked_line].text_end_offset,
          "editor End used the preceding visual line after a wrapped pointer click");

    adopt();
    layout = fixture.text_layout(key);
    click(first_cluster_point(layout, clicked_line), 52);
    static_cast<void>(fixture.input_.key("up"));
    check(ui::utf16_offset_for_utf8_byte(content, snapshot().caret) ==
              layout.lines[clicked_line - 1U].text_start_offset,
          "editor Up started from the preceding line at a shared wrap offset");
    static_cast<void>(fixture.input_.key("down"));
    check(ui::utf16_offset_for_utf8_byte(content, snapshot().caret) ==
              layout.lines[clicked_line].text_start_offset,
          "editor Down did not return along the pointer-seeded caret x goal");
    static_cast<void>(fixture.input_.key("down"));
    check(ui::utf16_offset_for_utf8_byte(content, snapshot().caret) ==
              layout.lines[clicked_line + 1U].text_start_offset,
          "repeated editor Down lost the pointer-seeded visual line or x goal");

    adopt();
    layout = fixture.text_layout(key);
    click(first_cluster_point(layout, clicked_line), 53);
    static_cast<void>(fixture.input_.key("down", ui::KeyModifiers{true, false, false, false}));
    const ui::TextEditorSnapshot shifted = snapshot();
    check(ui::utf16_offset_for_utf8_byte(content, shifted.selection_start) ==
                  layout.lines[clicked_line].text_start_offset &&
              ui::utf16_offset_for_utf8_byte(content, shifted.selection_end) ==
                  layout.lines[clicked_line + 1U].text_start_offset &&
              ui::utf16_offset_for_utf8_byte(content, shifted.caret) ==
                  layout.lines[clicked_line + 1U].text_start_offset,
          "editor Shift+Down lost its pointer anchor or wrapped-line affinity");

    adopt();
    layout = fixture.text_layout(key);
    const ui::Point drag_anchor = first_cluster_point(layout, clicked_line - 1U);
    const ui::Point drag_focus = first_cluster_point(layout, clicked_line);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{drag_anchor, ui::PointerEventType::press, 54, 0},
        ui::PointerInputEvent{drag_focus, ui::PointerEventType::move, 54, 0},
        ui::PointerInputEvent{drag_focus, ui::PointerEventType::release, 54, 0},
    }));
    const ui::TextEditorSnapshot dragged = snapshot();
    check(ui::utf16_offset_for_utf8_byte(content, dragged.selection_start) ==
                  layout.lines[clicked_line - 1U].text_start_offset &&
              ui::utf16_offset_for_utf8_byte(content, dragged.selection_end) ==
                  layout.lines[clicked_line].text_start_offset &&
              ui::utf16_offset_for_utf8_byte(content, dragged.caret) ==
                  layout.lines[clicked_line].text_start_offset,
          "wrapped editor drag lost its anchor or pointer-updated line affinity");
    static_cast<void>(fixture.input_.key("home"));
    check(ui::utf16_offset_for_utf8_byte(content, snapshot().caret) ==
              layout.lines[clicked_line].text_start_offset,
          "editor Home ignored the final wrapped drag placement line");
}

void test_editor_text_scroll(InputFixture& fixture) {
    constexpr std::string_view line_key = "editor.scroll.line";
    constexpr std::string_view area_key = "editor.scroll.area";
    const std::string line_text =
        "#minecraft:diamond_ores, #minecraft:iron_ores, #minecraft:gold_ores";
    std::string area_text;
    for (int index = 0; index < 12; ++index)
        area_text += "line " + std::to_string(index) + " of a wrapped editor text\n";
    ui::DescriptionNode::Properties line = sized(90.0, 24.0);
    line.emplace("text", runtime::ExpressionValue(runtime::Value(line_text)));
    ui::DescriptionNode::Properties area = sized(90.0, 48.0);
    area.emplace("text", runtime::ExpressionValue(runtime::Value(area_text)));
    const std::shared_ptr<const ui::DescriptionNode> description =
        node("Panel", "editor.scroll.root",
             {
                 node("TextBox", std::string(line_key), {}, std::move(line)),
                 node("TextArea", std::string(area_key), {}, std::move(area)),
             },
             sized(300.0, 200.0, "COLUMN"));
    fixture.adopt(description);

    const auto identity = [&fixture](const std::string_view key) {
        const ui::RetainedNode* retained = fixture.tree_.find_key(key);
        check(retained != nullptr, "scrolling editor fixture was not retained");
        return retained->identity();
    };
    const auto scroll = [&fixture, &identity](const std::string_view key) {
        return fixture.input_.editor_scroll(identity(key));
    };
    const auto caret_visible = [&fixture, &identity](const std::string_view key,
                                                     const bool multiline) {
        const ui::Rect viewport = fixture.bounds(key).content_bounds;
        const std::optional<ui::TextEditorSnapshot> editor =
            fixture.input_.editor_snapshot(identity(key));
        check(editor.has_value(), "scrolling editor lost its editor state");
        const ui::TextLayout layout = fixture.text_layout(
            key, editor->text, ui::editable_text_layout_options(viewport, multiline));
        const ui::Point origin = ui::editable_text_origin(
            viewport, layout, multiline, fixture.input_.editor_scroll(identity(key)));
        const ui::Rect caret =
            ui::text_layout_caret_rect(layout, origin, editor->text, editor->caret);
        return caret.x >= viewport.x - 0.5 && caret.right() <= viewport.right() + 0.5 &&
               (!multiline ||
                (caret.y >= viewport.y - 0.5 && caret.bottom() <= viewport.bottom() + 0.5));
    };
    const auto click = [&fixture](const std::string_view key, const std::int32_t pointer_id) {
        const ui::Point point = center(fixture.bounds(key).bounds);
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{point, ui::PointerEventType::press, pointer_id, 0},
            ui::PointerInputEvent{point, ui::PointerEventType::release, pointer_id, 0},
        }));
    };

    const ui::Rect line_viewport = fixture.bounds(line_key).content_bounds;
    const ui::TextLayout single = fixture.text_layout(
        line_key, line_text, ui::editable_text_layout_options(line_viewport, false));
    check(single.lines.size() == 1U && single.shaped.metrics.width > line_viewport.width,
          "single-line editor text wrapped or the fixture does not overflow");

    click(line_key, 61);
    check(fixture.focused(line_key), "single-line editor did not take focus");
    static_cast<void>(fixture.input_.key("end"));
    static_cast<void>(fixture.frame(description));
    check(scroll(line_key).x > 0.0 && scroll(line_key).y == 0.0 && caret_visible(line_key, false),
          "single-line editor did not scroll horizontally to its caret at End");
    static_cast<void>(fixture.input_.key("home"));
    static_cast<void>(fixture.frame(description));
    check(scroll(line_key).x == 0.0 && caret_visible(line_key, false),
          "single-line editor did not scroll back to its caret at Home");
    static_cast<void>(fixture.input_.key("end"));
    static_cast<void>(fixture.frame(description));
    check(scroll(line_key).x > 0.0, "single-line editor lost its End scroll");

    click(area_key, 62);
    check(fixture.focused(area_key), "multi-line editor did not take focus");
    static_cast<void>(fixture.frame(description));
    check(scroll(line_key).x == 0.0, "unfocused single-line editor did not show its start");

    static_cast<void>(fixture.input_.key("end", ui::KeyModifiers{false, true, false, false}));
    static_cast<void>(fixture.frame(description));
    const ui::Point bottom = scroll(area_key);
    check(bottom.y > 0.0 && caret_visible(area_key, true),
          "multi-line editor did not scroll vertically to its caret at Ctrl+End");

    static_cast<void>(fixture.input_.enqueue_scroll(
        ui::ScrollInputEvent{center(fixture.bounds(area_key).bounds), 0.0, 1.0}));
    static_cast<void>(fixture.input_.process_queued());
    const ui::Point wheeled = scroll(area_key);
    check(wheeled.y < bottom.y, "wheel over overflowing multi-line text did not scroll it");
    static_cast<void>(fixture.frame(description));
    check(scroll(area_key).y == wheeled.y,
          "multi-line editor snapped back to its unchanged caret after a wheel scroll");
}

void test_static_text_container_owner_transition(InputFixture& fixture) {
    ui::DescriptionNode::Properties first = sized(160.0, 30.0);
    first.emplace("text", runtime::ExpressionValue(runtime::Value("alpha")));
    first.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
    first.emplace("selectionContainer", runtime::ExpressionValue(runtime::Value("document")));
    ui::DescriptionNode::Properties second = sized(160.0, 30.0);
    second.emplace("text", runtime::ExpressionValue(runtime::Value("omega")));
    second.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
    second.emplace("selectionContainer", runtime::ExpressionValue(runtime::Value("document")));
    fixture.adopt(node("Panel", "owner.root",
                       {
                           node("Text", "owner.first", {}, std::move(first)),
                           node("Text", "owner.second", {}, std::move(second)),
                       },
                       sized(180.0, 80.0, "COLUMN")));
    const ui::RetainedNode* first_node = fixture.tree_.find_key("owner.first");
    const ui::RetainedNode* second_node = fixture.tree_.find_key("owner.second");
    check(first_node != nullptr && second_node != nullptr, "owner fixture was not retained");
    const ui::Rect first_bounds = fixture.bounds("owner.first").content_bounds;
    const ui::Rect second_bounds = fixture.bounds("owner.second").content_bounds;
    const ui::Point first_point{
        first_bounds.x + 4.0,
        first_bounds.y + first_bounds.height * 0.5,
    };
    const ui::Point second_point{
        second_bounds.x + 28.0,
        second_bounds.y + second_bounds.height * 0.5,
    };
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{first_point, ui::PointerEventType::press, 41, 0},
        ui::PointerInputEvent{first_point, ui::PointerEventType::release, 41, 0},
    }));
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{
            second_point,
            ui::PointerEventType::press,
            42,
            0,
            ui::KeyModifiers{true, false, false, false},
        },
        ui::PointerInputEvent{
            second_point,
            ui::PointerEventType::release,
            42,
            0,
            ui::KeyModifiers{true, false, false, false},
        },
    }));
    const std::optional<ui::StaticTextSelectionSnapshot> first_range =
        fixture.input_.static_text_selection_snapshot(first_node->identity());
    const std::optional<ui::StaticTextSelectionSnapshot> second_range =
        fixture.input_.static_text_selection_snapshot(second_node->identity());
    check(first_range.has_value() && second_range.has_value() &&
              first_range->selection_start != first_range->selection_end &&
              second_range->selection_start != second_range->selection_end,
          "Shift press within one selection container replaced instead of extending its owner");
}

[[nodiscard]] runtime::Value choices() {
    return runtime::Value(std::vector<runtime::Value>{
        object({{"id", runtime::Value("one")}, {"label", runtime::Value("One")}}),
        object({{"id", runtime::Value("two")}, {"label", runtime::Value("Two")}}),
        object({{"id", runtime::Value("three")}, {"label", runtime::Value("Three")}}),
    });
}

[[nodiscard]] runtime::Value choices_with_disabled_first() {
    return runtime::Value(std::vector<runtime::Value>{
        object({
            {"id", runtime::Value("one")},
            {"label", runtime::Value("One")},
            {"enabled", runtime::Value(false)},
        }),
        object({{"id", runtime::Value("two")}, {"label", runtime::Value("Two")}}),
        object({{"id", runtime::Value("three")}, {"label", runtime::Value("Three")}}),
    });
}

void test_nested_editor_does_not_activate_section(InputFixture& fixture) {
    ui::DescriptionNode::Properties chip = sized(260.0, 34.0);
    chip.emplace("defaultValues",
                 runtime::ExpressionValue(runtime::Value(std::vector<runtime::Value>{
                     runtime::Value("alpha"), runtime::Value("beta")})));
    chip.emplace("placeholder", runtime::ExpressionValue(runtime::Value("Type a tag")));
    ui::DescriptionNode::Properties section = sized(300.0, 100.0, "COLUMN");
    section.emplace("label", runtime::ExpressionValue(runtime::Value("Details")));
    section.emplace("defaultExpanded", runtime::ExpressionValue(runtime::Value(true)));
    fixture.adopt(node("Section", "nested.section",
                       {
                           node("ChipInput", "nested.editor", {}, std::move(chip)),
                       },
                       std::move(section)));

    const ui::Point editor = center(fixture.bounds("nested.editor").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{editor, ui::PointerEventType::press, 39, 0},
        ui::PointerInputEvent{editor, ui::PointerEventType::release, 39, 0},
    }));
    const ui::RetainedNode* retained_section = fixture.tree_.find_key("nested.section");
    check(fixture.input_.focused_key() == std::optional<std::string_view>{"nested.editor"},
          "nested editor press did not retain editor focus");
    check(retained_section != nullptr && retained_section->retained_value("$expanded") == nullptr,
          "nested editor click escaped into its Section ancestor");

    static_cast<void>(fixture.input_.key("space"));
    static_cast<void>(fixture.input_.text(" "));
    const ui::RetainedNode* retained_editor = fixture.tree_.find_key("nested.editor");
    const std::string* draft = retained_editor != nullptr
                                   ? fixture.input_.edited_text(retained_editor->identity())
                                   : nullptr;
    check(retained_section->retained_value("$expanded") == nullptr,
          "nested editor Space escaped into its Section ancestor");
    check(draft != nullptr && *draft == " ", "nested editor did not accept Space text input");
}

void test_slider_pointer_matches_rendered_track(InputFixture& fixture) {
    ui::DescriptionNode::Properties properties;
    properties.emplace("defaultValue", runtime::ExpressionValue(runtime::Value(40.0)));
    properties.emplace("min", runtime::ExpressionValue(runtime::Value(0.0)));
    properties.emplace("max", runtime::ExpressionValue(runtime::Value(100.0)));
    properties.emplace("step", runtime::ExpressionValue(runtime::Value(1.0)));
    properties.emplace("$layout", runtime::ExpressionValue(object({
                                      {"height", runtime::Value(24.0)},
                                      {"indicatorInset", runtime::Value(12.0)},
                                      {"kind", runtime::Value("PANEL")},
                                      {"width", runtime::Value(240.0)},
                                  })));
    fixture.adopt(node("Slider", "slider.geometry", {}, std::move(properties)));

    const ui::Rect bounds = fixture.bounds("slider.geometry").bounds;
    const double track_width = bounds.width - 24.0;
    const auto click_fraction = [&fixture, &bounds, track_width](const double fraction,
                                                                 const std::int32_t pointer_id) {
        const ui::Point point{
            bounds.x + 12.0 + track_width * fraction,
            bounds.y + bounds.height * 0.5,
        };
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{point, ui::PointerEventType::press, pointer_id, 0},
            ui::PointerInputEvent{point, ui::PointerEventType::release, pointer_id, 0},
        }));
    };
    const auto value = [&fixture]() -> double {
        const ui::RetainedNode* slider = fixture.tree_.find_key("slider.geometry");
        const runtime::Value* retained =
            slider != nullptr ? slider->retained_value("$value") : nullptr;
        return retained != nullptr && retained->number() != nullptr ? *retained->number() : -1.0;
    };

    click_fraction(0.4, 40);
    check(std::abs(value() - 40.0) <= 0.0001,
          "clicking the rendered Slider thumb changed its value");
    click_fraction(0.556, 41);
    check(std::abs(value() - 56.0) <= 0.0001,
          "Slider pointer input ignored its rendered track or authored step");

    const auto point_at = [&bounds, track_width](const double fraction) {
        return ui::Point{
            bounds.x + 12.0 + track_width * fraction,
            bounds.y + bounds.height * 0.5,
        };
    };
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{point_at(0.85), ui::PointerEventType::move, 42, 0},
    }));
    check(std::abs(value() - 56.0) <= 0.0001,
          "hovering across a Slider changed its value without an active press");

    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{point_at(0.25), ui::PointerEventType::press, 43, 0},
        ui::PointerInputEvent{point_at(0.75), ui::PointerEventType::move, 43, 0},
        ui::PointerInputEvent{point_at(0.75), ui::PointerEventType::release, 43, 0},
    }));
    check(std::abs(value() - 75.0) <= 0.0001,
          "dragging an actively pressed Slider did not update its value");
    static_cast<void>(fixture.input_.key("home"));
    check(value() == 0.0, "Slider Home did not reach its minimum");
    static_cast<void>(fixture.input_.key("end"));
    check(value() == 100.0, "Slider End did not reach its maximum");
    static_cast<void>(fixture.input_.key("pagedown"));
    check(value() == 90.0, "Slider Page Down did not move by ten steps");
    static_cast<void>(fixture.input_.key("pageup"));
    check(value() == 100.0, "Slider Page Up did not clamp at the maximum");
    // Hosts that spell the key with an underscore reach the same handler.
    static_cast<void>(fixture.input_.key("page_down"));
    check(value() == 90.0, "Slider ignored a host's page_down");
}

// A focused ZoomView zooms with + and -, pans with the arrows and goes back to its content's own
// size with 0, easing each time and building on a zoom that is still easing.
void test_zoom_view_keys(InputFixture& fixture) {
    fixture.adopt(node("ZoomView", "zoom.keys",
                       {node("Panel", "zoom.keys.content", {}, sized(800.0, 600.0, "CANVAS"))}));
    const ui::Point inside = center(fixture.bounds("zoom.keys").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{inside, ui::PointerEventType::press, 61, 0},
        ui::PointerInputEvent{inside, ui::PointerEventType::release, 61, 0},
    }));
    check(fixture.focused("zoom.keys"), "pressing a ZoomView did not focus it");
    const auto target = [&fixture]() -> std::optional<ui::ZoomViewport> {
        const ui::RetainedNode* zoom = fixture.tree_.find_key("zoom.keys");
        const runtime::Value* value =
            zoom != nullptr ? zoom->retained_value("$zoomTarget") : nullptr;
        if (value == nullptr || value->object() == nullptr)
            return std::nullopt;
        const auto number = [value](const std::string_view name) {
            const runtime::Value* field = value->field(name);
            return field != nullptr && field->number() != nullptr ? *field->number() : 0.0;
        };
        return ui::ZoomViewport{number("x"), number("y"), number("zoom")};
    };

    static_cast<void>(fixture.input_.key("="));
    std::optional<ui::ZoomViewport> eased = target();
    check(eased.has_value() && std::abs(eased->zoom - 1.2) < 1.0e-9,
          "= did not zoom a focused ZoomView in by a notch");
    // About the middle of the view: the content point at its centre stays there.
    const ui::Rect shown = fixture.bounds("zoom.keys").content_bounds;
    const ui::Point middle{shown.x + shown.width * 0.5, shown.y + shown.height * 0.5};
    check(eased.has_value() && std::abs(eased->x - (middle.x - middle.x * 1.2)) < 1.0e-6 &&
              std::abs(eased->y - (middle.y - middle.y * 1.2)) < 1.0e-6,
          "a key zoom did not keep the middle of the view in place");
    static_cast<void>(fixture.input_.key("+"));
    eased = target();
    check(eased.has_value() && std::abs(eased->zoom - 1.44) < 1.0e-9,
          "+ did not build on the zoom still easing");
    static_cast<void>(fixture.input_.key("-"));
    static_cast<void>(fixture.input_.key("-"));
    eased = target();
    check(eased.has_value() && std::abs(eased->zoom - 1.0) < 1.0e-9,
          "- did not zoom back out by a notch each");
    static_cast<void>(fixture.input_.key("right"));
    const std::optional<ui::ZoomViewport> panned = target();
    check(panned.has_value() && eased.has_value() &&
              std::abs(panned->x - (eased->x - 40.0)) < 1.0e-9 &&
              std::abs(panned->y - eased->y) < 1.0e-9,
          "Right did not pan the content left by a step");
    static_cast<void>(fixture.input_.key("down", ui::KeyModifiers{.shift = true}));
    const std::optional<ui::ZoomViewport> far = target();
    check(far.has_value() && panned.has_value() && std::abs(far->y - (panned->y - 200.0)) < 1.0e-9,
          "Shift+Down did not pan five steps");
    static_cast<void>(fixture.input_.key("0"));
    eased = target();
    check(eased.has_value() && eased->zoom == 1.0 && eased->x == 0.0 && eased->y == 0.0,
          "0 did not go back to the content's own size");
}

void test_choice_semantics(InputFixture& fixture) {
    ui::DescriptionNode::Properties controlled = sized(240.0, 32.0);
    controlled.emplace("tabs", runtime::ExpressionValue(choices()));
    controlled.emplace("selectedId", runtime::ExpressionValue(runtime::Value("one")));
    controlled.emplace("defaultSelectedId", runtime::ExpressionValue(runtime::Value("three")));
    ui::DescriptionNode::Properties retained = sized(240.0, 32.0);
    retained.emplace("tabs", runtime::ExpressionValue(choices()));
    retained.emplace("defaultSelectedId", runtime::ExpressionValue(runtime::Value("one")));
    ui::DescriptionNode::Properties authored_default = sized(240.0, 32.0);
    authored_default.emplace("options", runtime::ExpressionValue(choices()));
    authored_default.emplace("defaultSelectedId",
                             runtime::ExpressionValue(runtime::Value("three")));
    ui::DescriptionNode::Properties retained_radio = sized(240.0, 64.0);
    retained_radio.emplace("options", runtime::ExpressionValue(choices()));
    retained_radio.emplace("defaultSelectedId", runtime::ExpressionValue(runtime::Value("one")));
    ui::DescriptionNode::Properties absent_default = sized(240.0, 32.0);
    absent_default.emplace("tabs", runtime::ExpressionValue(choices_with_disabled_first()));
    ui::DescriptionNode::Properties disabled_select = sized(240.0, 32.0);
    disabled_select.emplace("options", runtime::ExpressionValue(choices_with_disabled_first()));
    disabled_select.emplace("defaultSelectedId",
                            runtime::ExpressionValue(runtime::Value("removed")));
    ui::DescriptionNode::Properties disabled_radio = sized(240.0, 64.0);
    disabled_radio.emplace("options", runtime::ExpressionValue(choices_with_disabled_first()));
    ui::DescriptionNode::Properties stale_retained = sized(240.0, 32.0);
    stale_retained.emplace("tabs", runtime::ExpressionValue(choices()));
    stale_retained.emplace("defaultSelectedId", runtime::ExpressionValue(runtime::Value("three")));
    ui::DescriptionNode::Properties invalid_controlled = sized(240.0, 64.0);
    invalid_controlled.emplace("options", runtime::ExpressionValue(choices()));
    invalid_controlled.emplace("selectedId", runtime::ExpressionValue(runtime::Value("removed")));
    invalid_controlled.emplace("defaultSelectedId",
                               runtime::ExpressionValue(runtime::Value("three")));
    fixture.adopt(
        node("Panel", "choice.root",
             {
                 node("Tabs", "choice.controlled", {}, std::move(controlled)),
                 node("Tabs", "choice.retained", {}, std::move(retained)),
                 node("Select", "choice.default", {}, std::move(authored_default)),
                 node("RadioGroup", "choice.radio", {}, std::move(retained_radio)),
                 node("Tabs", "choice.absent-default", {}, std::move(absent_default)),
                 node("Select", "choice.disabled-select", {}, std::move(disabled_select)),
                 node("RadioGroup", "choice.disabled-radio", {}, std::move(disabled_radio)),
                 node("Tabs", "choice.stale-retained", {}, std::move(stale_retained)),
                 node("RadioGroup", "choice.invalid-controlled", {}, std::move(invalid_controlled)),
             },
             sized(300.0, 420.0, "COLUMN")));
    ui::RetainedNode* retained_tabs = fixture.tree_.find_key("choice.retained");
    ui::RetainedNode* radio = fixture.tree_.find_key("choice.radio");
    ui::RetainedNode* stale = fixture.tree_.find_key("choice.stale-retained");
    ui::RetainedNode* invalid = fixture.tree_.find_key("choice.invalid-controlled");
    check(retained_tabs != nullptr && radio != nullptr && stale != nullptr && invalid != nullptr,
          "choice fixtures were not retained");
    static_cast<void>(fixture.tree_.set_retained_value(retained_tabs->identity(), "$selectedId",
                                                       runtime::Value("two"),
                                                       ui::DirtyReason::properties));
    static_cast<void>(fixture.tree_.set_retained_value(
        radio->identity(), "$selectedId", runtime::Value("two"), ui::DirtyReason::properties));
    static_cast<void>(fixture.tree_.set_retained_value(
        stale->identity(), "$selectedId", runtime::Value("removed"), ui::DirtyReason::properties));
    static_cast<void>(fixture.tree_.set_retained_value(
        invalid->identity(), "$selectedId", runtime::Value("two"), ui::DirtyReason::properties));

    const auto require_choice = [&fixture](const std::string_view key, const std::string_view id,
                                           const std::size_t index) {
        const ui::RetainedNode* retained = fixture.tree_.find_key(key);
        const std::optional<ui::EffectiveChoice> selection =
            retained != nullptr ? ui::effective_choice(*retained) : std::nullopt;
        check(selection.has_value() && selection->id == id && selection->index == index,
              std::string("wrong canonical choice for ") + std::string(key));
    };
    require_choice("choice.controlled", "one", 0U);
    require_choice("choice.retained", "two", 1U);
    require_choice("choice.default", "three", 2U);
    require_choice("choice.radio", "two", 1U);
    require_choice("choice.absent-default", "one", 0U);
    require_choice("choice.disabled-select", "two", 1U);
    require_choice("choice.disabled-radio", "two", 1U);
    require_choice("choice.stale-retained", "three", 2U);
    require_choice("choice.invalid-controlled", "two", 1U);
    check(ui::choice_is_controlled(*invalid),
          "an invalid controlled choice stopped being controlled for mutation purposes");

    ui::SemanticsEngine semantics(fixture.widgets_, fixture.behaviors_);
    static_cast<void>(
        semantics.update(fixture.tree_, fixture.commands_, fixture.input_, "interaction-residual"));
    const data::JsonValue* controlled_semantics =
        semantics.find(fixture.tree_.find_key("choice.controlled")->identity());
    const data::JsonValue* retained_semantics = semantics.find(retained_tabs->identity());
    const data::JsonValue* default_semantics =
        semantics.find(fixture.tree_.find_key("choice.default")->identity());
    const data::JsonValue* radio_semantics = semantics.find(radio->identity());
    const data::JsonValue* absent_semantics =
        semantics.find(fixture.tree_.find_key("choice.absent-default")->identity());
    const data::JsonValue* disabled_select_semantics =
        semantics.find(fixture.tree_.find_key("choice.disabled-select")->identity());
    const data::JsonValue* disabled_radio_semantics =
        semantics.find(fixture.tree_.find_key("choice.disabled-radio")->identity());
    const data::JsonValue* stale_semantics = semantics.find(stale->identity());
    const data::JsonValue* invalid_semantics = semantics.find(invalid->identity());
    check(controlled_semantics != nullptr && semantic_value_text(*controlled_semantics) == "one" &&
              retained_semantics != nullptr && semantic_value_text(*retained_semantics) == "two" &&
              default_semantics != nullptr && semantic_value_text(*default_semantics) == "three" &&
              radio_semantics != nullptr && semantic_value_text(*radio_semantics) == "two" &&
              absent_semantics != nullptr && semantic_value_text(*absent_semantics) == "one" &&
              disabled_select_semantics != nullptr &&
              semantic_value_text(*disabled_select_semantics) == "two" &&
              disabled_radio_semantics != nullptr &&
              semantic_value_text(*disabled_radio_semantics) == "two" &&
              stale_semantics != nullptr && semantic_value_text(*stale_semantics) == "three" &&
              invalid_semantics != nullptr && semantic_value_text(*invalid_semantics) == "two",
          "choice semantics diverged from canonical option-aware resolution");

    ui::RetainedNode* controlled_tabs = fixture.tree_.find_key("choice.controlled");
    const std::vector<ui::WidgetSubtarget> controlled_targets =
        fixture.input_.subtargets(controlled_tabs->identity());
    const std::vector<ui::WidgetSubtarget> retained_targets =
        fixture.input_.subtargets(retained_tabs->identity());
    check(controlled_targets.size() == 3U && retained_targets.size() == 3U,
          "choice fixtures did not expose their input rows");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{center(controlled_targets[1].bounds), ui::PointerEventType::press, 4,
                              0},
        ui::PointerInputEvent{center(controlled_targets[1].bounds), ui::PointerEventType::release,
                              4, 0},
    }));
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{center(retained_targets[2].bounds), ui::PointerEventType::press, 5,
                              0},
        ui::PointerInputEvent{center(retained_targets[2].bounds), ui::PointerEventType::release, 5,
                              0},
    }));
    const std::vector<ui::WidgetSubtarget> invalid_targets =
        fixture.input_.subtargets(invalid->identity());
    check(invalid_targets.size() == 3U, "invalid controlled choice rows were unavailable");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{center(invalid_targets[2].bounds), ui::PointerEventType::press, 6, 0},
        ui::PointerInputEvent{center(invalid_targets[2].bounds), ui::PointerEventType::release, 6,
                              0},
    }));
    check(controlled_tabs->retained_value("$selectedId") == nullptr &&
              ui::effective_choice_id(*controlled_tabs) == "one" &&
              ui::effective_choice_id(*retained_tabs) == "three" &&
              invalid->retained_value("$selectedId") != nullptr &&
              invalid->retained_value("$selectedId")->string() != nullptr &&
              *invalid->retained_value("$selectedId")->string() == "two" &&
              ui::effective_choice_id(*invalid) == "two",
          "choice input did not share controlled/uncontrolled resolution with semantics");
}

void test_tooltip_disclosure(InputFixture& fixture) {
    ui::DescriptionNode::Properties tooltip = sized(180.0, 36.0);
    tooltip.emplace("text", runtime::ExpressionValue(runtime::Value("Delayed help")));
    tooltip.emplace("showDelay",
                    runtime::ExpressionValue(runtime::Value(runtime::DurationValue{100'000'000})));
    tooltip.emplace("hideDelay",
                    runtime::ExpressionValue(runtime::Value(runtime::DurationValue{50'000'000})));
    fixture.adopt(node("Panel", "tooltip.root",
                       {
                           node("Tooltip", "tooltip.owner",
                                {
                                    node("Probe", "tooltip.anchor", {}, sized(180.0, 36.0)),
                                },
                                std::move(tooltip)),
                           node("Probe", "tooltip.outside", {}, sized(180.0, 36.0)),
                       },
                       sized(300.0, 120.0, "COLUMN")));
    ui::RetainedNode* owner = fixture.tree_.find_key("tooltip.owner");
    check(owner != nullptr, "Tooltip fixture was not retained");
    check(ui::tooltip_show_delay_nanos(*owner) == 100'000'000 &&
              ui::tooltip_hide_delay_nanos(*owner) == 50'000'000,
          "Tooltip did not decode its public duration-valued delay contract");
    const ui::Point anchor = center(fixture.bounds("tooltip.anchor").bounds);
    const ui::Point outside{600.0, 450.0};

    fixture.input_.publish_frame_time(0);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{anchor, ui::PointerEventType::move, 0, 0},
    }));
    check(!ui::tooltip_disclosure_visible(*owner) && fixture.input_.requires_frame_advance(),
          "Tooltip ignored its authored show delay or failed to schedule the deadline");
    fixture.input_.publish_frame_time(99'000'000);
    check(!ui::tooltip_disclosure_visible(*owner), "Tooltip opened before its configured delay");
    fixture.input_.publish_frame_time(100'000'000);
    check(ui::tooltip_disclosure_visible(*owner) && !fixture.input_.requires_frame_advance(),
          "Tooltip did not settle after its configured show delay");
    const std::vector<ui::WidgetSubtarget> targets = fixture.input_.subtargets(owner->identity());
    const auto popup =
        std::ranges::find(targets, std::string("$tooltip"), &ui::WidgetSubtarget::id);
    check(popup != targets.end() && popup->detached &&
              popup->z_index == ui::detached_overlay_tooltip_z,
          "visible Tooltip did not join shared detached overlay hit ordering");

    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{center(popup->bounds), ui::PointerEventType::move, 0, 0},
    }));
    check(ui::tooltip_disclosure_visible(*owner) && !fixture.input_.requires_frame_advance(),
          "Tooltip anchor/overlay hover bridge scheduled a spurious hide");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{outside, ui::PointerEventType::move, 0, 0},
    }));
    check(ui::tooltip_disclosure_visible(*owner) && fixture.input_.requires_frame_advance(),
          "Tooltip ignored its authored hide delay");
    fixture.input_.publish_frame_time(149'000'000);
    check(ui::tooltip_disclosure_visible(*owner), "Tooltip hid before its configured delay");
    fixture.input_.publish_frame_time(150'000'000);
    check(!ui::tooltip_disclosure_visible(*owner) && !fixture.input_.requires_frame_advance(),
          "hidden Tooltip retained perpetual frame work after settling");

    fixture.input_.publish_frame_time(200'000'000);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{anchor, ui::PointerEventType::press, 7, 0},
        ui::PointerInputEvent{anchor, ui::PointerEventType::release, 7, 0},
    }));
    fixture.input_.publish_frame_time(300'000'000);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{outside, ui::PointerEventType::move, 0, 0},
    }));
    fixture.input_.publish_frame_time(350'000'000);
    check(!ui::tooltip_disclosure_visible(*owner) && !fixture.input_.requires_frame_advance(),
          "Pointer focus kept an old tooltip visible after leaving its anchor");

    fixture.input_.publish_frame_time(400'000'000);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{anchor, ui::PointerEventType::move, 0, 0},
    }));
    fixture.input_.publish_frame_time(450'000'000);
    const ui::Point shifted{anchor.x + 8.0, anchor.y};
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{shifted, ui::PointerEventType::move, 0, 0},
    }));
    fixture.input_.publish_frame_time(549'000'000);
    check(!ui::tooltip_disclosure_visible(*owner),
          "Moving inside an anchor did not restart the idle-hover deadline");
    fixture.input_.publish_frame_time(550'000'000);
    check(ui::tooltip_disclosure_visible(*owner),
          "Stationary hover did not reveal the tooltip after its restarted deadline");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{shifted, ui::PointerEventType::press, 8, 0},
    }));
    fixture.input_.publish_frame_time(800'000'000);
    check(!ui::tooltip_disclosure_visible(*owner),
          "A pressed control disclosed an idle-hover tooltip");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{outside, ui::PointerEventType::release, 8, 0},
    }));
    fixture.input_.publish_frame_time(1'000'000'000);
    check(!ui::tooltip_disclosure_visible(*owner) && !fixture.input_.hovered(owner->identity()),
          "Release outside the anchor restored stale press-origin hover");
}

void test_manipulation_slop(InputFixture& fixture) {
    std::size_t clicks = 0U;
    ui::WidgetInputPhase probe;
    probe.click = [&clicks](ui::WidgetInputScope&) {
        ++clicks;
        return true;
    };
    probe.focusable = true;
    fixture.widgets_.register_input_phase("Probe", std::move(probe));

    const auto behavior = [](std::string id, runtime::Value options) {
        return std::vector<ui::DescriptionBehavior>{ui::DescriptionBehavior{
            std::move(id),
            true,
            std::move(options),
            nullptr,
        }};
    };
    fixture.adopt(node(
        "Panel", "manipulation.root",
        {
            node("Probe", "manipulation.move",
                 {
                     node("Text", "manipulation.move.glyph", {}, sized(40.0, 20.0)),
                 },
                 sized(160.0, 36.0),
                 behavior("strata.movable", object({{"bounds", runtime::Value("none")}}))),
            node("Probe", "manipulation.resize",
                 {
                     node("Text", "manipulation.resize.glyph", {}, sized(40.0, 20.0)),
                 },
                 sized(160.0, 36.0),
                 behavior("strata.resize", object({
                                               {"horizontal", runtime::Value(true)},
                                               {"vertical", runtime::Value(true)},
                                           }))),
            node("Panel", "manipulation.pane",
                 {
                     node("Probe", "manipulation.divider", {}, sized(12.0, 36.0),
                          behavior("strata.split-handle",
                                   object({
                                       {"paneKey",
                                        runtime::Value(runtime::KeyValue{"manipulation.pane"})},
                                   }))),
                 },
                 sized(200.0, 36.0, "ROW")),
        },
        sized(300.0, 160.0, "COLUMN")));

    const auto exercise = [&fixture](const ui::Point origin, const std::int32_t pointer_id,
                                     const double delta) {
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{origin, ui::PointerEventType::press, pointer_id, 0},
            ui::PointerInputEvent{
                ui::Point{origin.x + delta, origin.y},
                ui::PointerEventType::move,
                pointer_id,
                0,
            },
            ui::PointerInputEvent{
                ui::Point{origin.x + delta, origin.y},
                ui::PointerEventType::release,
                pointer_id,
                0,
            },
        }));
    };

    ui::RetainedNode* movable = fixture.tree_.find_key("manipulation.move");
    ui::RetainedNode* resize = fixture.tree_.find_key("manipulation.resize");
    ui::RetainedNode* pane = fixture.tree_.find_key("manipulation.pane");
    check(movable != nullptr && resize != nullptr && pane != nullptr,
          "manipulation fixtures were not retained");
    exercise(center(fixture.bounds("manipulation.move").bounds), 11, 3.0);
    check(movable->retained_value("strata.movement.offset") == nullptr && clicks == 1U,
          "under-slop move mutated state or stole click activation");
    exercise(center(fixture.bounds("manipulation.move.glyph").bounds), 12, 8.0);
    check(movable->retained_value("strata.movement.offset") != nullptr && clicks == 1U,
          "over-slop move failed to claim exactly once and suppress activation");

    exercise(center(fixture.bounds("manipulation.resize").bounds), 13, 3.0);
    check(resize->retained_value("strata.gesture.runtimeSize") == nullptr && clicks == 2U,
          "under-slop resize mutated dimensions or stole click activation");
    exercise(center(fixture.bounds("manipulation.resize.glyph").bounds), 14, 8.0);
    check(resize->retained_value("strata.gesture.runtimeSize") != nullptr && clicks == 2U,
          "over-slop resize failed to claim or mutate dimensions");

    exercise(center(fixture.bounds("manipulation.divider").bounds), 15, 3.0);
    check(pane->retained_value("strata.gesture.splitRatio") == nullptr && clicks == 3U,
          "under-slop split mutated ratio or stole click activation");
    exercise(center(fixture.bounds("manipulation.divider").bounds), 16, 8.0);
    check(pane->retained_value("strata.gesture.splitRatio") != nullptr && clicks == 3U,
          "over-slop split failed to claim or mutate its pane ratio");
}

// Snapping to the container and to a movable sibling (beside it at the gap), the moved events a
// host keeps the placement from, guides only while dragging, and arrow-key nudges.
void test_movable_snapping(InputFixture& fixture) {
    const auto movable = [](runtime::Value options) {
        return std::vector<ui::DescriptionBehavior>{
            ui::DescriptionBehavior{"strata.movable", true, std::move(options), nullptr}};
    };
    const auto options = [] {
        return object({{"snap", object({
                                    {"distance", runtime::Value(6.0)},
                                    {"gap", runtime::Value(4.0)},
                                })}});
    };
    fixture.adopt(node("Panel", "snap.root",
                       {
                           node("Panel", "snap.area",
                                {
                                    node("Panel", "snap.anchor", {}, sized(100.0, 40.0), movable(options())),
                                    node("Panel", "snap.moving", {}, sized(60.0, 30.0), movable(options())),
                                },
                                sized(400.0, 300.0, "ROW")),
                       },
                       sized(640.0, 480.0, "COLUMN")));
    ui::RetainedNode* moving = fixture.tree_.find_key("snap.moving");
    check(moving != nullptr, "snapping fixture was not retained");
    const ui::Rect start = fixture.bounds("snap.moving").bounds;
    const ui::Point grip = center(start);
    const auto offset = [moving] {
        const runtime::Value* value = moving->retained_value("strata.movement.offset");
        const runtime::Value* x = value != nullptr ? value->field("x") : nullptr;
        const runtime::Value* y = value != nullptr ? value->field("y") : nullptr;
        return ui::Point{x != nullptr && x->number() != nullptr ? *x->number() : 0.0,
                         y != nullptr && y->number() != nullptr ? *y->number() : 0.0};
    };
    const auto guides_shown = [moving] {
        const runtime::Value* guides = moving->retained_value("strata.movement.guides");
        return guides != nullptr && guides->object() != nullptr;
    };
    const auto last_moved = [](const ui::InputOperationResult& result) -> const data::JsonValue* {
        const data::JsonValue* found = nullptr;
        for (const data::JsonValue& event : result.events) {
            const data::JsonValue* type = event.find("type");
            if (type != nullptr && type->string() != nullptr && *type->string() == "moved")
                found = event.find("value");
        }
        return found;
    };
    const auto number_at = [](const data::JsonValue* value, const std::string_view path_a,
                              const std::string_view path_b = {}) {
        const data::JsonValue* current = value != nullptr ? value->find(path_a) : nullptr;
        if (current != nullptr && !path_b.empty())
            current = current->find(path_b);
        return current != nullptr && current->number() != nullptr ? *current->number() : -1.0;
    };

    // Right edge dragged to 3 px from the container's: it lands on it.
    ui::InputOperationResult result = fixture.pointer({
        ui::PointerInputEvent{grip, ui::PointerEventType::press, 21, 0},
        ui::PointerInputEvent{ui::Point{grip.x + 80.0, grip.y + 80.0}, ui::PointerEventType::move, 21, 0},
        ui::PointerInputEvent{ui::Point{grip.x + 237.0, grip.y + 120.0}, ui::PointerEventType::move, 21, 0},
    });
    check(guides_shown(), "a snapped drag showed no guides");
    check(number_at(last_moved(result), "guideX") == 400.0, "the guide is not the container edge");
    result = fixture.pointer({
        ui::PointerInputEvent{ui::Point{grip.x + 237.0, grip.y + 120.0}, ui::PointerEventType::release, 21, 0},
    });
    const data::JsonValue* ended = last_moved(result);
    check(offset().x == 400.0 - start.right() && offset().y == 120.0,
          "the right edge did not snap to the container's");
    check(ended != nullptr && ended->find("phase") != nullptr &&
              *ended->find("phase")->string() == "end" &&
              number_at(ended, "bounds", "x") == 340.0 && number_at(ended, "container", "width") == 400.0,
          "the finished move did not report where it shows");
    check(!guides_shown(), "guides outlived the drag");

    // Left edge 3 px from the anchor's right plus the gap: it lands beside it. This fixture has no
    // motion runtime, so hit testing sees layout bounds: the press goes where the box was laid out.
    const ui::Point from = grip;
    const double target = fixture.bounds("snap.anchor").bounds.right() + 4.0 + 3.0;
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{from, ui::PointerEventType::press, 22, 0},
        ui::PointerInputEvent{ui::Point{from.x - 90.0, from.y - 40.0}, ui::PointerEventType::move, 22, 0},
        ui::PointerInputEvent{ui::Point{from.x + (target - (start.x + offset().x)), from.y - 60.0},
                              ui::PointerEventType::move, 22, 0},
        ui::PointerInputEvent{ui::Point{from.x + (target - (start.x + offset().x)), from.y - 60.0},
                              ui::PointerEventType::release, 22, 0},
    }));
    check(start.x + offset().x == fixture.bounds("snap.anchor").bounds.right() + 4.0 && offset().y == 60.0,
          "the box did not snap beside its sibling at the gap");

    // Arrow keys nudge a focused movable and report a finished move.
    check(fixture.focused("snap.moving"), "pressing a movable did not focus it");
    const ui::Point before = offset();
    result = fixture.input_.key("right", ui::KeyModifiers{});
    check(offset().x == before.x + 1.0 && last_moved(result) != nullptr, "an arrow key did not nudge");
    static_cast<void>(fixture.input_.key("down", ui::KeyModifiers{.shift = true}));
    check(offset().y == before.y + 10.0, "Shift did not nudge ten times as far");
}

// A released movable flings on with its velocity and springs onto the snap point that velocity
// carries it toward, past the nearest one; a drag past rubber-band limits is resisted and springs
// back inside them; the settle is reported with the snap index.
void test_movable_fling_snap_and_bounds(InputFixture& fixture) {
    const auto movable = [](runtime::Value options) {
        return std::vector<ui::DescriptionBehavior>{
            ui::DescriptionBehavior{"strata.movable", true, std::move(options), nullptr}};
    };
    fixture.adopt(node("Panel", "fling.root",
                       {
                           node("Panel", "fling.card", {}, sized(100.0, 100.0),
                                movable(object({
                                    {"axis", runtime::Value("x")},
                                    {"bounds", runtime::Value("none")},
                                    {"fling", runtime::Value(true)},
                                    {"snapX", runtime::Value(std::vector<runtime::Value>{
                                                  runtime::Value(-300.0), runtime::Value(0.0),
                                                  runtime::Value(300.0)})},
                                }))),
                           node("Panel", "fling.sheet", {}, sized(100.0, 100.0),
                                movable(object({
                                    {"axis", runtime::Value("y")},
                                    {"bounds", object({{"minY", runtime::Value(0.0)},
                                                       {"maxY", runtime::Value(100.0)}})},
                                }))),
                       },
                       sized(640.0, 480.0, "ROW")));
    const auto offset = [&fixture](const std::string_view key) {
        const ui::RetainedNode* node = fixture.tree_.find_key(key);
        const runtime::Value* value =
            node != nullptr ? node->retained_value("strata.movement.offset") : nullptr;
        const runtime::Value* x = value != nullptr ? value->field("x") : nullptr;
        const runtime::Value* y = value != nullptr ? value->field("y") : nullptr;
        return ui::Point{x != nullptr && x->number() != nullptr ? *x->number() : 0.0,
                         y != nullptr && y->number() != nullptr ? *y->number() : 0.0};
    };
    std::vector<data::JsonValue> settles;
    const auto collect = [&settles](const ui::InputOperationResult& result) {
        for (const data::JsonValue& event : result.events) {
            const data::JsonValue* value = event.find("value");
            const data::JsonValue* phase = value != nullptr ? value->find("phase") : nullptr;
            if (phase != nullptr && phase->string() != nullptr && *phase->string() == "settle")
                settles.push_back(*value);
        }
    };
    // Frames until the movable comes to rest, 60 a second.
    std::int64_t now = 1'000'000'000;
    const auto settle = [&](const std::int64_t until) {
        for (; now < until; now += 16'666'667) {
            fixture.input_.publish_frame_time(now);
            collect(fixture.input_.advance_frame());
        }
    };

    // Released at 60 px moving 2000 px/s: friction carries it toward 560, so it passes the nearest
    // snap point (0) and springs onto 300.
    // One event a frame, as a pointer moving across frames arrives (moves within one frame are
    // coalesced into the last).
    const auto deliver = [&](const ui::PointerInputEvent& event) {
        fixture.input_.publish_frame_time(event.timestamp_nanos);
        collect(fixture.pointer({event}));
    };
    const ui::Point grip = center(fixture.bounds("fling.card").bounds);
    deliver(ui::PointerInputEvent{grip, ui::PointerEventType::press, 31, 0, {}, {}, now});
    for (int step = 1; step <= 3; ++step) {
        deliver(ui::PointerInputEvent{ui::Point{grip.x + 20.0 * step, grip.y + 4.0 * step},
                                      ui::PointerEventType::move,
                                      31,
                                      0,
                                      {},
                                      {},
                                      now + step * 10'000'000});
    }
    deliver(ui::PointerInputEvent{ui::Point{grip.x + 60.0, grip.y + 12.0},
                                  ui::PointerEventType::release,
                                  31,
                                  0,
                                  {},
                                  {},
                                  now + 32'000'000});
    check(offset("fling.card").y == 0.0, "axis x let the card move vertically");
    now += 33'000'000;
    settle(now + 3'000'000'000);
    check(std::abs(offset("fling.card").x - 300.0) < 1.0e-6,
          "the flick did not carry the card onto the far snap point: x = " +
              std::to_string(offset("fling.card").x));
    check(!settles.empty() && settles.back().find("snapped") != nullptr &&
              settles.back().find("snapped")->number() != nullptr &&
              *settles.back().find("snapped")->number() == 2.0,
          "the settle did not report snap point 2");

    // The same move released at rest snaps to the nearest point instead.
    settles.clear();
    const ui::Point moved = ui::Point{grip.x + 300.0, grip.y};
    deliver(ui::PointerInputEvent{moved, ui::PointerEventType::press, 32, 0, {}, {}, now});
    deliver(ui::PointerInputEvent{ui::Point{moved.x - 60.0, moved.y},
                                  ui::PointerEventType::move,
                                  32,
                                  0,
                                  {},
                                  {},
                                  now + 10'000'000});
    deliver(ui::PointerInputEvent{ui::Point{moved.x - 60.0, moved.y},
                                  ui::PointerEventType::release,
                                  32,
                                  0,
                                  {},
                                  {},
                                  now + 400'000'000});
    now += 401'000'000;
    settle(now + 3'000'000'000);
    check(std::abs(offset("fling.card").x - 300.0) < 1.0e-6,
          "a slow release did not settle on the nearest snap point");

    // Past its limits a drag is resisted: 200 px beyond maxY shows well short of that, and the
    // release springs back onto the limit.
    const ui::Point handle = center(fixture.bounds("fling.sheet").bounds);
    deliver(ui::PointerInputEvent{handle, ui::PointerEventType::press, 33, 0, {}, {}, now});
    deliver(ui::PointerInputEvent{ui::Point{handle.x, handle.y + 150.0},
                                  ui::PointerEventType::move,
                                  33,
                                  0,
                                  {},
                                  {},
                                  now + 10'000'000});
    deliver(ui::PointerInputEvent{ui::Point{handle.x, handle.y + 300.0},
                                  ui::PointerEventType::move,
                                  33,
                                  0,
                                  {},
                                  {},
                                  now + 20'000'000});
    const double stretched = offset("fling.sheet").y;
    check(stretched > 100.0 && stretched < 200.0,
          "a drag past the limit was not resisted like a rubber band: y = " +
              std::to_string(stretched));
    deliver(ui::PointerInputEvent{ui::Point{handle.x, handle.y + 300.0},
                                  ui::PointerEventType::release,
                                  33,
                                  0,
                                  {},
                                  {},
                                  now + 200'000'000});
    now += 201'000'000;
    settle(now + 3'000'000'000);
    check(std::abs(offset("fling.sheet").y - 100.0) < 1.0e-6,
          "the rubber band did not spring back onto the limit");
}

// After layout, `strata.scroll` reports a scroll's offset, progress, velocity and extents whenever
// they change (and once more at rest, with no velocity), and `strata.size` reports a node's
// laid-out size whenever it changes, but not again when it only flips back to the size before.
void test_layout_observers(InputFixture& fixture) {
    const auto observer = [](const std::string_view id) {
        return std::vector<ui::DescriptionBehavior>{
            ui::DescriptionBehavior{std::string(id), true, object({}), nullptr}};
    };
    const auto describe = [&](const double width) {
        return node(
            "Panel", "observe.root",
            {
                node("Panel", "observe.scroll",
                     {node("Panel", "observe.content", {}, sized(200.0, 1000.0))},
                     {{"$layout", layout(200.0, 200.0, "SCROLL")}}, observer("strata.scroll")),
                node("Panel", "observe.sized", {}, sized(width, 50.0), observer("strata.size")),
            },
            sized(640.0, 480.0, "COLUMN"));
    };
    std::int64_t now = 2'000'000'000;
    const auto relayout = [&](std::shared_ptr<const ui::DescriptionNode> description) {
        if (description != nullptr) {
            fixture.input_.begin_tree_update();
            static_cast<void>(fixture.tree_.reconcile(std::move(description)));
            static_cast<void>(fixture.input_.prepare(fixture.tree_));
        }
        const ui::LayoutResult& result =
            fixture.layout_.layout(fixture.tree_, ui::LayoutEnvironment{
                                                      0U,
                                                      ui::Rect{0.0, 0.0, 640.0, 480.0},
                                                      1.0,
                                                      {},
                                                      ui::PointSnapPolicy::nearest,
                                                      ui::RectangleSnapPolicy::outward,
                                                      false,
                                                  });
        fixture.input_.publish_layout(result);
        fixture.input_.publish_frame_time(now);
        return fixture.input_.after_layout();
    };
    const auto reported = [](const ui::InputOperationResult& result,
                             const std::string_view type) -> std::optional<data::JsonValue> {
        std::optional<data::JsonValue> found;
        for (const data::JsonValue& event : result.events) {
            const data::JsonValue* kind = event.find("type");
            if (kind != nullptr && kind->string() != nullptr && *kind->string() == type)
                found = *event.find("value");
        }
        return found;
    };
    const auto number = [](const std::optional<data::JsonValue>& value,
                           const std::string_view name) {
        const data::JsonValue* field = value.has_value() ? value->find(name) : nullptr;
        return field != nullptr && field->number() != nullptr ? *field->number() : -1.0;
    };

    fixture.adopt(describe(300.0));
    ui::InputOperationResult result = relayout(nullptr);
    std::optional<data::JsonValue> scroll = reported(result, "scroll-changed");
    std::optional<data::JsonValue> size = reported(result, "size-changed");
    check(scroll.has_value() && number(scroll, "y") == 0.0 && number(scroll, "progressY") == 0.0 &&
              number(scroll, "contentHeight") == 1000.0 &&
              number(scroll, "viewportHeight") == 200.0,
          "the first layout did not report the scroll's extents");
    check(size.has_value() && number(size, "width") == 300.0 && number(size, "height") == 50.0,
          "the first layout did not report the node's size");
    check(!reported(relayout(nullptr), "size-changed").has_value() &&
              !reported(relayout(nullptr), "scroll-changed").has_value(),
          "an unchanged layout was reported again");

    // Scrolled 400 of its 800 px range in 20 ms: half way, at 20000 px/s.
    now += 20'000'000;
    ui::InputOperationResult moved;
    check(fixture.input_.scroll_to("observe.scroll", ui::Point{0.0, 400.0}, moved),
          "the scroll did not move");
    scroll = reported(relayout(nullptr), "scroll-changed");
    check(number(scroll, "y") == 400.0 && number(scroll, "progressY") == 0.5 &&
              std::abs(number(scroll, "velocityY") - 20000.0) < 1.0e-6,
          "a moved scroll did not report its progress and velocity");
    // At rest, once more without velocity.
    now += 200'000'000;
    scroll = reported(relayout(nullptr), "scroll-changed");
    check(scroll.has_value() && number(scroll, "velocityY") == 0.0 && number(scroll, "y") == 400.0,
          "a scroll at rest was not reported once more with no velocity");
    check(!reported(relayout(nullptr), "scroll-changed").has_value(),
          "a scroll at rest kept reporting");

    // A size change is reported; one straight back to the size before, at once, is not.
    now += 1'000'000;
    size = reported(relayout(describe(420.0)), "size-changed");
    check(number(size, "width") == 420.0, "a resized node did not report its new size");
    now += 1'000'000;
    check(!reported(relayout(describe(300.0)), "size-changed").has_value(),
          "a size flipping straight back was reported (a feedback loop)");
    now += 500'000'000;
    size = reported(relayout(describe(360.0)), "size-changed");
    check(number(size, "width") == 360.0, "a later size change was not reported");
}

// A backdrop observer resolves host luma reports into a bistable light/dark reading: it changes
// only past a threshold, holds between them, believes a change only once it has persisted for the
// dwell, and emits once per change.
void test_backdrop_observer(InputFixture& fixture) {
    const auto observer = [](const bool enabled) {
        return std::vector<ui::DescriptionBehavior>{ui::DescriptionBehavior{
            "strata.backdrop", enabled,
            object({{"light", runtime::Value(0.6)}, {"dark", runtime::Value(0.4)},
                    {"dwell", runtime::Value(2.0)}}),
            nullptr}};
    };
    fixture.adopt(node("Panel", "backdrop.root",
                       {
                           node("Panel", "backdrop.glass", {}, sized(100.0, 40.0), observer(true)),
                           node("Panel", "backdrop.off", {}, sized(100.0, 40.0), observer(false)),
                       },
                       sized(640.0, 480.0, "COLUMN")));
    ui::RetainedNode* glass = fixture.tree_.find_key("backdrop.glass");
    ui::RetainedNode* off = fixture.tree_.find_key("backdrop.off");
    check(glass != nullptr && off != nullptr, "backdrop fixture was not retained");
    const auto report = [&fixture](const std::uint64_t probe, const double luminance) {
        ui::InputOperationResult result;
        fixture.input_.observe_backdrop(probe, luminance, result);
        return result.events.size();
    };
    const auto light = [glass]() -> std::optional<bool> {
        const runtime::Value* value = glass->retained_value("strata.backdrop.light");
        return value != nullptr && value->boolean() != nullptr
                   ? std::optional<bool>(*value->boolean())
                   : std::nullopt;
    };
    check(!light().has_value(), "an unreported observer already had a reading");
    check(report(glass->identity(), 0.45) == 0U && light() == std::optional<bool>(false),
          "a first dark reading was announced although the node already presents as dark");
    check(report(glass->identity(), 0.55) == 0U && light() == std::optional<bool>(false),
          "a reading between the thresholds did not hold");
    check(report(glass->identity(), 0.7) == 0U && light() == std::optional<bool>(false),
          "a single light reading flipped before the dwell");
    check(report(glass->identity(), 0.7) == 1U && light() == std::optional<bool>(true),
          "a light reading held for the dwell did not flip and emit once");
    check(report(glass->identity(), 0.5) == 0U && light() == std::optional<bool>(true),
          "a light reading did not hold between the thresholds");
    check(report(glass->identity(), 0.3) == 0U && report(glass->identity(), 0.5) == 0U &&
              report(glass->identity(), 0.3) == 0U && light() == std::optional<bool>(true),
          "an interrupted dark streak counted toward the dwell");
    check(report(glass->identity(), 0.3) == 1U && light() == std::optional<bool>(false),
          "a dark reading held for the dwell did not flip back and emit once");
    check(report(off->identity(), 0.9) == 0U &&
              off->retained_value("strata.backdrop.light") == nullptr,
          "a disabled observer accepted a report");
    check(report(glass->identity() + 100'000U, 0.9) == 0U, "an unknown probe was not ignored");
}

// A scale handle inside a movable card: the handle owns its gesture, the scale follows the pointer's
// progress away from the fixed anchor (clamped and stepped), and arrow keys step it.
void test_scale_handle(InputFixture& fixture) {
    const auto attach = [](std::string id, runtime::Value options) {
        return std::vector<ui::DescriptionBehavior>{
            ui::DescriptionBehavior{std::move(id), true, std::move(options), nullptr}};
    };
    fixture.adopt(node("Panel", "scale.root",
                       {
                           node("Panel", "scale.card",
                                {
                                    node("Panel", "scale.body", {}, sized(90.0, 50.0)),
                                    node("Panel", "scale.handle", {}, sized(10.0, 10.0),
                                         attach("strata.scale", object({
                                                                    {"value", runtime::Value(1.0)},
                                                                    {"min", runtime::Value(0.5)},
                                                                    {"max", runtime::Value(2.0)},
                                                                    {"step", runtime::Value(0.05)},
                                                                    {"anchor", object({{"x", runtime::Value(0.0)},
                                                                                       {"y", runtime::Value(0.0)}})},
                                                                }))),
                                },
                                sized(100.0, 50.0, "ROW"),
                                attach("strata.movable", object({{"bounds", runtime::Value("none")}}))),
                       },
                       sized(640.0, 480.0, "COLUMN")));
    ui::RetainedNode* card = fixture.tree_.find_key("scale.card");
    check(card != nullptr, "scale fixture was not retained");
    const ui::Rect box = fixture.bounds("scale.card").bounds;
    const ui::Point grip = center(fixture.bounds("scale.handle").bounds);
    const auto scaled = [](const ui::InputOperationResult& result, const std::string_view phase) {
        double found = -1.0;
        for (const data::JsonValue& event : result.events) {
            const data::JsonValue* type = event.find("type");
            const data::JsonValue* value = event.find("value");
            if (type == nullptr || type->string() == nullptr || *type->string() != "scaled" || value == nullptr)
                continue;
            if (*value->find("phase")->string() == phase)
                found = *value->find("scale")->number();
        }
        return found;
    };
    // Half as far again along the line from the card's top-left through the press: 1.5.
    const ui::Point target{box.x + (grip.x - box.x) * 1.5, box.y + (grip.y - box.y) * 1.5};
    ui::InputOperationResult result = fixture.pointer({
        ui::PointerInputEvent{grip, ui::PointerEventType::press, 31, 0},
        ui::PointerInputEvent{ui::Point{grip.x + 20.0, grip.y}, ui::PointerEventType::move, 31, 0},
        ui::PointerInputEvent{target, ui::PointerEventType::move, 31, 0},
        ui::PointerInputEvent{target, ui::PointerEventType::release, 31, 0},
    });
    const runtime::Value* moved = card->retained_value("strata.movement.offset");
    check(moved == nullptr || moved->object() == nullptr, "the enclosing movable took the handle's drag");
    check(std::abs(scaled(result, "end") - 1.5) < 1e-9, "the handle did not scale along its drag");
    result = fixture.pointer({
        ui::PointerInputEvent{grip, ui::PointerEventType::press, 32, 0},
        ui::PointerInputEvent{ui::Point{grip.x + 300.0, grip.y + 60.0}, ui::PointerEventType::move, 32, 0},
        ui::PointerInputEvent{ui::Point{grip.x + 300.0, grip.y + 60.0}, ui::PointerEventType::release, 32, 0},
    });
    check(scaled(result, "end") == 2.0, "the scale was not clamped to its maximum");
    check(fixture.focused("scale.handle"), "pressing the handle did not focus it");
    result = fixture.input_.key("up", ui::KeyModifiers{});
    check(std::abs(scaled(result, "end") - 1.05) < 1e-9, "an arrow key did not step the scale");
}

void test_passive_descendant_activation(InputFixture& fixture) {
    const auto activate = [&fixture](std::string message) {
        return std::vector<ui::DescriptionBehavior>{ui::DescriptionBehavior{
            "strata.activate",
            true,
            runtime::Value{},
            fixture.notification_action(std::move(message)),
        }};
    };
    ui::DescriptionNode::Properties text = sized(180.0, 28.0);
    text.emplace("text", runtime::ExpressionValue(runtime::Value("Passive label")));
    text.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
    fixture.adopt(node("Panel", "passive.card",
                       {
                           node("Text", "passive.label", {}, std::move(text)),
                       },
                       sized(220.0, 48.0), activate("passive card")));

    const std::size_t notifications_before = fixture.notifications_.size();
    const ui::Point label = center(fixture.bounds("passive.label").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{label, ui::PointerEventType::press, 31, 0},
        ui::PointerInputEvent{label, ui::PointerEventType::release, 31, 0},
    }));
    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "passive.card" &&
              fixture.notifications_.size() == notifications_before + 1U,
          "passive Text masked its interactive ancestor's focus or activation");

    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{label, ui::PointerEventType::press, 32, 0},
        ui::PointerInputEvent{
            ui::Point{label.x + 12.0, label.y},
            ui::PointerEventType::move,
            32,
            0,
        },
        ui::PointerInputEvent{
            ui::Point{label.x + 12.0, label.y},
            ui::PointerEventType::release,
            32,
            0,
        },
    }));
    check(fixture.notifications_.size() == notifications_before + 1U,
          "static-text drag selection leaked into ancestor activation");

    ui::DescriptionNode::Properties double_text = sized(180.0, 28.0);
    double_text.emplace("text", runtime::ExpressionValue(runtime::Value("Double-click label")));
    fixture.adopt(node("Panel", "double.card",
                       {
                           node("Text", "double.label", {}, std::move(double_text)),
                       },
                       sized(220.0, 48.0),
                       {
                           ui::DescriptionBehavior{
                               "strata.activate",
                               true,
                               object({{"clickCount", runtime::Value(2.0)}}),
                               fixture.notification_action("double card"),
                           },
                       }));
    const ui::Point double_label = center(fixture.bounds("double.label").bounds);
    fixture.input_.publish_frame_time(100'000'000);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{double_label, ui::PointerEventType::press, 35, 0},
        ui::PointerInputEvent{double_label, ui::PointerEventType::release, 35, 0},
    }));
    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "double.card" &&
              fixture.notifications_.size() == notifications_before + 1U,
          "first passive-descendant click moved focus back to selectable Text");
    fixture.input_.publish_frame_time(200'000'000);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{double_label, ui::PointerEventType::press, 36, 0},
        ui::PointerInputEvent{double_label, ui::PointerEventType::release, 36, 0},
    }));
    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "double.card" &&
              fixture.notifications_.size() == notifications_before + 2U,
          "passive-descendant double click did not activate exactly once");

    ui::DescriptionNode::Properties button = sized(160.0, 32.0);
    button.emplace("label", runtime::ExpressionValue(runtime::Value("Nested control")));
    button.emplace("onClick",
                   runtime::ExpressionValue(fixture.notification_action("nested control")));
    fixture.adopt(node("Panel", "boundary.card",
                       {
                           node("Button", "boundary.button", {}, std::move(button)),
                       },
                       sized(220.0, 48.0), activate("outer card")));
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{
            center(fixture.bounds("boundary.button").bounds),
            ui::PointerEventType::press,
            33,
            0,
        },
        ui::PointerInputEvent{
            center(fixture.bounds("boundary.button").bounds),
            ui::PointerEventType::release,
            33,
            0,
        },
    }));
    check(fixture.notifications_.size() == notifications_before + 3U &&
              fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "boundary.button",
          "nested interactive control leaked activation or focus to its ancestor");

    ui::DescriptionNode::Properties included_text = sized(180.0, 28.0);
    included_text.emplace("text", runtime::ExpressionValue(runtime::Value("Included label")));
    fixture.adopt(node("Panel", "included.card",
                       {
                           node("Text", "included.label", {}, std::move(included_text)),
                       },
                       sized(220.0, 48.0),
                       {
                           ui::DescriptionBehavior{
                               "strata.activate",
                               true,
                               object({{"includeDescendants", runtime::Value(true)}}),
                               nullptr,
                           },
                       }));
    const ui::Point included_label = center(fixture.bounds("included.label").bounds);
    const ui::InputOperationResult included_result = fixture.pointer({
        ui::PointerInputEvent{included_label, ui::PointerEventType::press, 34, 0},
        ui::PointerInputEvent{included_label, ui::PointerEventType::release, 34, 0},
    });
    check(std::ranges::count_if(included_result.events,
                                [](const data::JsonValue& event) {
                                    const data::JsonValue* type = event.find("type");
                                    return type != nullptr && type->string() != nullptr &&
                                           *type->string() == "activated";
                                }) == 1,
          "explicit descendant activation dispatched through both capture and passive bubble");
}

void test_release_target_activation(InputFixture& fixture) {
    const auto button = [&fixture](std::string key, const bool authored) {
        ui::DescriptionNode::Properties label = sized(140.0, 24.0);
        label.emplace("text", runtime::ExpressionValue(runtime::Value("ordinary button label")));
        auto properties = sized(180.0, 36.0);
        if (!authored)
            properties.emplace("onClick",
                               runtime::ExpressionValue(fixture.notification_action(key)));
        return node(
            authored ? "Panel" : "Button", key,
            {node("Text", key + ".label", {}, std::move(label))}, std::move(properties),
            authored
                ? std::vector<ui::DescriptionBehavior>{{"strata.activate", true, runtime::Value{},
                                                        fixture.notification_action(key)}}
                : std::vector<ui::DescriptionBehavior>{});
    };
    fixture.adopt(node("Panel", "release.root",
                       {button("release.native", false), button("release.authored", true)},
                       sized(300.0, 150.0, "COLUMN")));
    const auto drag_click = [&fixture](const ui::Point from, const ui::Point to) {
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{from, ui::PointerEventType::press, 71, 0},
            ui::PointerInputEvent{to, ui::PointerEventType::move, 71, 0},
            ui::PointerInputEvent{to, ui::PointerEventType::release, 71, 0},
        }));
    };
    const std::size_t before = fixture.notifications_.size();
    const ui::Point native = center(fixture.bounds("release.native.label").bounds);
    const ui::Point authored = center(fixture.bounds("release.authored.label").bounds);
    drag_click(native, {native.x + 35.0, native.y});
    drag_click(authored, {authored.x + 35.0, authored.y});
    check(fixture.notifications_.size() == before + 2U,
          "Movement within a stock/authored button cancelled its click");
    for (const std::string_view key : {"release.native.label", "release.authored.label"}) {
        check(
            !fixture.input_.static_text_selection_snapshot(fixture.tree_.find_key(key)->identity()),
            "An ordinary label intercepted a button drag as text selection");
    }
    drag_click(native, authored);
    drag_click(authored, {290.0, 140.0});
    check(fixture.notifications_.size() == before + 2U,
          "Releasing over another control or background activated the pressed button");
}

void test_select_inside_modal(InputFixture& fixture) {
    auto select = sized(140.0, 32.0);
    select.emplace(
        "options",
        runtime::ExpressionValue(runtime::Value(std::vector<runtime::Value>{
            object({{"id", runtime::Value("toggle")}, {"label", runtime::Value("Toggle")}}),
            object({{"id", runtime::Value("start")}, {"label", runtime::Value("Start")}}),
        })));
    auto modal = sized(640.0, 480.0);
    modal.insert_or_assign("$layout", runtime::ExpressionValue(object({
                                          {"width", runtime::Value(640.0)},
                                          {"height", runtime::Value(480.0)},
                                          {"zIndex", runtime::Value(10'000.0)},
                                      })));
    modal.emplace("open", runtime::ExpressionValue(runtime::Value(true)));
    modal.emplace("onDismiss",
                  runtime::ExpressionValue(fixture.notification_action("dismiss modal")));
    fixture.adopt(
        node("Modal", "choice.modal",
             {node("Panel", "choice.dialog",
                   {node("Select", "choice.select", {}, std::move(select))}, sized(320.0, 180.0))},
             std::move(modal)));
    const ui::Point trigger = center(fixture.bounds("choice.select").bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{trigger, ui::PointerEventType::press, 72, 0},
        ui::PointerInputEvent{{trigger.x + 12.0, trigger.y}, ui::PointerEventType::move, 72, 0},
        ui::PointerInputEvent{{trigger.x + 12.0, trigger.y}, ui::PointerEventType::release, 72, 0},
    }));
    const ui::RetainedNode* select_node = fixture.tree_.find_key("choice.select");
    const auto choices = fixture.input_.subtargets(select_node->identity());
    const auto option = std::ranges::find_if(choices, [](const ui::WidgetSubtarget& target) {
        return target.kind == ui::WidgetSubtargetKind::choice && target.value.string() != nullptr &&
               *target.value.string() == "start";
    });
    check(option != choices.end(), "The modal backdrop swallowed its Select trigger");
    const ui::Point item = center(option->bounds);
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{item, ui::PointerEventType::press, 73, 0},
        ui::PointerInputEvent{item, ui::PointerEventType::release, 73, 0},
    }));
    const auto selected = ui::effective_choice(*select_node);
    check(selected.has_value() && selected->id == "start",
          "A detached choice inside the modal could not be selected");
    const std::size_t before = fixture.notifications_.size();
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{{500.0, 300.0}, ui::PointerEventType::press, 74, 0},
        ui::PointerInputEvent{{530.0, 310.0}, ui::PointerEventType::move, 74, 0},
        ui::PointerInputEvent{{530.0, 310.0}, ui::PointerEventType::release, 74, 0},
    }));
    check(fixture.notifications_.size() == before + 1U,
          "Movement within the backdrop cancelled modal dismissal");
}

void test_banner_semantics(InputFixture& fixture) {
    const auto banner = [&fixture](std::string key, const bool action, const bool dismissible,
                                   const bool collapsed = false) {
        ui::DescriptionNode::Properties properties =
            collapsed ? sized(0.0, 0.0, "SPACER") : sized(280.0, 46.0);
        properties.emplace("message", runtime::ExpressionValue(runtime::Value(key)));
        properties.emplace("dismissible", runtime::ExpressionValue(runtime::Value(dismissible)));
        if (action && !collapsed) {
            properties.emplace(
                "action", runtime::ExpressionValue(fixture.notification_action(key + " action")));
            properties.emplace("actionLabel", runtime::ExpressionValue(runtime::Value("Act")));
        }
        return node("Banner", std::move(key), {}, std::move(properties));
    };
    const auto banner_tree = [&banner](const bool collapsed) {
        return node("Panel", "banner.root",
                    {
                        banner("banner.none", false, false),
                        banner("banner.action", true, false),
                        banner("banner.dismiss", false, true, collapsed),
                        banner("banner.both", true, true, collapsed),
                    },
                    sized(320.0, 200.0, "COLUMN"));
    };
    const std::shared_ptr<const ui::DescriptionNode> initial_description = banner_tree(false);
    const std::shared_ptr<const ui::DescriptionNode> collapsed_description = banner_tree(true);
    fixture.adopt(initial_description);
    ui::SemanticsEngine semantics(fixture.widgets_, fixture.behaviors_);
    static_cast<void>(
        semantics.update(fixture.tree_, fixture.commands_, fixture.input_, "interaction-residual"));
    const auto actions = [&fixture, &semantics](const std::string_view key) {
        const ui::RetainedNode* retained = fixture.tree_.find_key(key);
        const data::JsonValue* value =
            retained != nullptr ? semantics.find(retained->identity()) : nullptr;
        return value != nullptr ? semantic_actions(*value) : std::vector<std::string>{};
    };
    check(actions("banner.none") == std::vector<std::string>{"focus"} &&
              actions("banner.action") == std::vector<std::string>{"activate", "focus"} &&
              actions("banner.dismiss") == std::vector<std::string>{"dismiss", "focus"} &&
              actions("banner.both") == std::vector<std::string>{"activate", "dismiss", "focus"},
          "Banner semantics advertised actions absent from its conditional input targets");
    const std::vector<ui::WidgetSubtarget> targets =
        fixture.input_.subtargets(fixture.tree_.find_key("banner.both")->identity());
    check(std::ranges::find(targets, std::string("$action"), &ui::WidgetSubtarget::id) !=
                  targets.end() &&
              std::ranges::find(targets, std::string("$dismiss"), &ui::WidgetSubtarget::id) !=
                  targets.end(),
          "Banner executable action/dismiss conditions diverged from its semantics");

    std::int32_t pointer_id = 20;
    const auto focus_banner = [&fixture, &pointer_id](const std::string_view key) {
        const ui::Rect bounds = fixture.bounds(key).bounds;
        const ui::Point message{bounds.x + 5.0, bounds.y + bounds.height * 0.5};
        static_cast<void>(fixture.pointer({
            ui::PointerInputEvent{message, ui::PointerEventType::press, pointer_id, 0},
            ui::PointerInputEvent{message, ui::PointerEventType::release, pointer_id, 0},
        }));
        ++pointer_id;
        check(fixture.input_.focused_key().has_value() && *fixture.input_.focused_key() == key,
              "Banner fixture did not receive focus");
    };
    const auto navigate = [&fixture](std::string direction) {
        std::vector<ui::SurfaceInputEvent> events;
        events.emplace_back(ui::NavigationInputEvent{std::move(direction)});
        static_cast<void>(fixture.input_.enqueue(std::move(events)));
        return fixture.input_.process_queued();
    };

    const std::size_t notification_count = fixture.notifications_.size();
    const auto action_target = std::ranges::find(targets, std::string(ui::banner_action_subtarget),
                                                 &ui::WidgetSubtarget::id);
    check(action_target != targets.end(), "Banner action subtarget was unavailable");
    static_cast<void>(fixture.pointer({
        ui::PointerInputEvent{center(action_target->bounds), ui::PointerEventType::press, 30, 0},
        ui::PointerInputEvent{center(action_target->bounds), ui::PointerEventType::release, 30, 0},
    }));
    check(fixture.notifications_.size() == notification_count + 1U,
          "pointer Banner action did not execute through its subtarget");

    focus_banner("banner.action");
    static_cast<void>(fixture.input_.key("enter"));
    check(fixture.notifications_.size() == notification_count + 2U,
          "Enter did not execute the applicable Banner action subtarget");

    ui::RetainedNode* dismiss = fixture.tree_.find_key("banner.dismiss");
    ui::RetainedNode* both = fixture.tree_.find_key("banner.both");
    check(dismiss != nullptr && both != nullptr, "Banner execution fixtures were not retained");
    focus_banner("banner.dismiss");
    static_cast<void>(fixture.input_.key("space"));
    check(dismiss->retained_value("$dismissed") != nullptr &&
              dismiss->retained_value("$dismissed")->boolean() != nullptr &&
              *dismiss->retained_value("$dismissed")->boolean(),
          "Space did not execute the applicable Banner dismiss subtarget");

    static_cast<void>(fixture.tree_.set_retained_value(
        both->identity(), std::string(ui::banner_active_subtarget_state),
        runtime::Value(std::string(ui::banner_dismiss_subtarget)), ui::DirtyReason::input));
    focus_banner("banner.both");
    static_cast<void>(fixture.input_.key("enter"));
    check(both->retained_value("$dismissed") != nullptr &&
              both->retained_value("$dismissed")->boolean() != nullptr &&
              *both->retained_value("$dismissed")->boolean() &&
              fixture.notifications_.size() == notification_count + 2U,
          "Banner keyboard activation ignored its retained applicable subtarget");

    focus_banner("banner.action");
    static_cast<void>(navigate("activate"));
    check(fixture.notifications_.size() == notification_count + 3U,
          "semantic/navigation activate did not execute Banner $action");
    static_cast<void>(fixture.tree_.set_retained_value(
        dismiss->identity(), "$dismissed", runtime::Value(false), ui::DirtyReason::layout));
    focus_banner("banner.dismiss");
    static_cast<void>(navigate("cancel"));
    check(dismiss->retained_value("$dismissed") != nullptr &&
              dismiss->retained_value("$dismissed")->boolean() != nullptr &&
              *dismiss->retained_value("$dismissed")->boolean(),
          "semantic/navigation dismiss did not execute Banner $dismiss");

    check(fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "banner.dismiss",
          "dismissed Banner was not focused before reconciliation regression");
    const InputFixture::FrameResult dismissed_frame = fixture.frame(collapsed_description);
    dismiss = fixture.tree_.find_key("banner.dismiss");
    check(dismiss != nullptr, "dismissed Banner identity was lost during reconciliation");
    check(!ui::project_banner(*dismiss).active && fixture.bounds("banner.dismiss").bounds.empty() &&
              fixture.input_.subtargets(dismiss->identity()).empty(),
          "dismissed Banner retained physical participation after its spacer reconciliation");
    check(!fixture.input_.focused_key().has_value() && !dismissed_frame.input.events.empty(),
          "focus sanitization retained a dismissed Banner");
    check(semantics.update(fixture.tree_, fixture.commands_, fixture.input_,
                           "interaction-residual") &&
              semantics.find(dismiss->identity()) == nullptr,
          "dismissed Banner remained visible or actionable in semantics");

    for (std::size_t traversal = 0U; traversal < 4U; ++traversal) {
        static_cast<void>(fixture.input_.key("tab"));
        const std::optional<std::string_view> focused = fixture.input_.focused_key();
        check(focused.has_value() && *focused != "banner.dismiss" && *focused != "banner.both",
              "dismissed Banner remained in traversal order");
    }
    static_cast<void>(
        semantics.update(fixture.tree_, fixture.commands_, fixture.input_, "interaction-residual"));

    const InputFixture::FrameResult settled = fixture.frame(collapsed_description);
    check(!settled.reconciliation.changed() && settled.layout.measured_nodes == 0U &&
              settled.layout.arranged_nodes == 0U && settled.input.events.empty() &&
              !semantics.update(fixture.tree_, fixture.commands_, fixture.input_,
                                "interaction-residual"),
          "dismissed Banner did not settle after reconciliation");
}

void check_paired(const ui::InputOperationResult& result, const std::string_view message) {
    check(result.events.size() == result.action_outcomes.size(), message);
    for (std::size_t index = 0U; index < result.events.size(); ++index) {
        const data::JsonValue* event = result.action_outcomes[index].find("event");
        check(event != nullptr && *event == result.events[index], message);
    }
}

void test_event_outcome_pairing(InputFixture& fixture) {
    ui::DescriptionNode::Properties button = sized(80.0, 32.0);
    button.emplace("onClick", runtime::ExpressionValue(fixture.framework_action(
                                  "focus.request",
                                  object({{"key", runtime::Value(runtime::KeyValue{"pair.field"})}}))));
    fixture.adopt(node("Panel", "pair.root",
                       {
                           node("Button", "pair.button", {}, std::move(button)),
                           node("Probe", "pair.field", {}, sized(80.0, 32.0)),
                       },
                       sized(300.0, 160.0, "COLUMN")));
    const ui::Point point = center(fixture.bounds("pair.button").bounds);
    const ui::InputOperationResult clicked = fixture.pointer({
        ui::PointerInputEvent{point, ui::PointerEventType::press, 300, 0},
        ui::PointerInputEvent{point, ui::PointerEventType::release, 300, 0},
    });
    check(fixture.focused("pair.field"), "Button focus.request did not move focus");
    // The click's outcome is produced after the focus move records its blur/focus pair; it must
    // still sit beside the click, not after the pairs its action recorded.
    const bool moved = std::ranges::any_of(clicked.events, [](const data::JsonValue& event) {
        const data::JsonValue* type = event.find("type");
        return type != nullptr && type->string() != nullptr && *type->string() == "focus-changed";
    });
    check(moved, "Button focus.request recorded no focus change");
    check_paired(clicked, "a nested focus move split an event from its action outcome");
}

[[nodiscard]] bool has_event(const ui::InputOperationResult& result, const std::string_view type,
                             const std::string_view field, const data::JsonValue& value) {
    return std::ranges::any_of(result.events, [&](const data::JsonValue& event) {
        const data::JsonValue* kind = event.find("type");
        const data::JsonValue* found = event.find(field);
        return kind != nullptr && kind->string() != nullptr && *kind->string() == type &&
               found != nullptr && *found == value;
    });
}

void test_removed_focused_editor_commits(InputFixture& fixture) {
    ui::DescriptionNode::Properties field = sized(160.0, 28.0);
    field.emplace("commitFormat", runtime::ExpressionValue(runtime::Value("uppercase")));
    const auto tree = [&field](const bool dialog) {
        std::vector<std::shared_ptr<const ui::DescriptionNode>> children{
            node("Probe", "removal.anchor", {}, sized(80.0, 32.0)),
        };
        if (dialog) {
            children.push_back(node("Panel", "removal.dialog",
                                    {node("TextBox", "removal.field", {}, field)},
                                    sized(200.0, 60.0, "COLUMN")));
        }
        return node("Panel", "removal.root", std::move(children), sized(300.0, 200.0, "COLUMN"));
    };
    fixture.adopt(tree(true));
    static_cast<void>(fixture.input_.click("removal.field"));
    check(fixture.focused("removal.field"), "removal fixture editor was not focused");
    static_cast<void>(fixture.input_.text("ab"));
    // Dismissing the dialog removes the focused editor before the router sees the new tree. It is
    // still blurred, and its uncommitted edit committed, as if focus had moved away first.
    const InputFixture::FrameResult removed = fixture.frame(tree(false));
    check(!fixture.input_.focused_key().has_value(), "removed editor kept focus");
    check(has_event(removed.input, "focus-changed", "focused", data::JsonValue(false)),
          "removed focused editor was not blurred");
    check(has_event(removed.input, "text-changed", "text", data::JsonValue("AB")),
          "removed focused editor dropped its uncommitted edit");
    check_paired(removed.input, "removed editor blur split an event from its outcome");
}

[[nodiscard]] std::unique_ptr<ui::Surface>
compiled_surface(runtime::ApplicationContext& application, const std::filesystem::path& resources,
                 const std::string& source, const std::string& root, const double width,
                 const double height, const bool reduced_motion = true) {
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{root + ".strata", source}, no_imports, 0U);
    if (!activation.activated()) {
        std::string message = "surface fixture '" + root + "' did not activate";
        for (const runtime::ActivationDiagnostic& diagnostic : activation.diagnostics)
            message += " [" + diagnostic.code + ": " + diagnostic.message + "]";
        throw std::runtime_error(message);
    }
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = static_cast<int>(width);
    environment.framebuffer_height = static_cast<int>(height);
    environment.logical_width = width;
    environment.logical_height = height;
    environment.reduced_motion = reduced_motion;
    return std::make_unique<ui::Surface>(
        root, application, runtime::LayerRole::overlay, root, environment,
        ui::TextEngine::load_control_font(
            resources, resource::ResourceId::parse("assets/strata/fonts/medium.ttf")));
}

void test_virtual_focused_row_stays_realized(const std::filesystem::path& resources) {
    std::string items;
    for (int index = 0; index < 120; ++index) {
        items += std::string(index == 0 ? "" : ", ") + "{ key: \"row." + std::to_string(index) +
                 "\", label: \"Row " + std::to_string(index) + "\" }";
    }
    const std::string source = R"(
component EditRow(item: record, key: key, index: number) {
  TextBox(key: key, text: item.label, layout: { width: 160, height: 28 })
}
overlay VirtualRows {
  Panel(key: "rows.root", layout: { kind: "COLUMN", width: { weight: 1 }, height: { weight: 1 } }) {
    VirtualList(
      key: "rows.list",
      items: [)" + items + R"(],
      itemTemplate: EditRow,
      itemExtent: 28,
      layout: { kind: "SCROLL", width: { weight: 1 }, height: 112 }
    )
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("virtual-rows", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "VirtualRows", 320.0, 200.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    static_cast<void>(surface->input().click("rows.list.$item.row.1"));
    static_cast<void>(surface->input().text("draft"));
    static_cast<void>(surface->frame(now += 16'000'000));
    const ui::RetainedNode* row = surface->tree().find_key("rows.list.$item.row.1");
    check(row != nullptr && surface->input().focused(row->identity()),
          "virtual row editor was not focused");
    const std::uint64_t identity = row->identity();

    ui::InputOperationResult scrolled;
    check(surface->input().scroll_to("rows.list", ui::Point{0.0, 28.0 * 100.0}, scrolled),
          "virtual list did not scroll");
    for (int frame = 0; frame < 3; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->tree().find_key("rows.list.$item.row.100") != nullptr,
          "virtual list did not realize its new window");
    // The focused row left the realized window but stays attached, focused and unedited.
    row = surface->tree().find_key("rows.list.$item.row.1");
    check(row != nullptr && row->identity() == identity,
          "virtual list released the focused row it scrolled away from");
    check(surface->input().focused(identity), "focus was lost when its row scrolled away");
    const std::optional<ui::TextEditorSnapshot> snapshot =
        surface->input().editor_snapshot(identity);
    check(snapshot.has_value() && snapshot->text == "Row 1draft",
          "the focused row's uncommitted edit was lost when it scrolled away");
    check(surface->tree().find_key("rows.list.$item.row.50") == nullptr,
          "keeping the focused row realized the rows between it and the window");

    // Once focus leaves, the row is released like any other row outside the window.
    static_cast<void>(surface->input().click("rows.list.$item.row.100"));
    for (int frame = 0; frame < 2; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->tree().find_key("rows.list.$item.row.1") == nullptr,
          "a row kept for focus outlived the focus that kept it");
}

[[nodiscard]] runtime::Value reveal_payload(const std::string& key, const bool focus,
                                            const double padding) {
    return object({
        {"focus", runtime::Value(focus)},
        {"key", runtime::Value(runtime::KeyValue{key})},
        {"padding", runtime::Value(padding)},
    });
}

void test_post_layout_reveal_reaches_layout(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Reveal {
  root Panel(key: "reveal.root", layout: { kind: "COLUMN", width: 320, height: 400 }) {
    Scroll(key: "reveal.outer", vertical: true, horizontal: false, layout: { width: 220, height: 200 }) {
      Panel(key: "reveal.outer.content", layout: { kind: "COLUMN", width: 200, height: 400 }) {
        Scroll(key: "reveal.inner", vertical: true, horizontal: false, layout: { width: 200, height: 100 }) {
          Panel(key: "reveal.inner.content", layout: { kind: "COLUMN", width: 180, height: 500 }) {
            Panel(key: "reveal.spacer", layout: { width: 180, height: 300 })
            TextBox(key: "reveal.target", text: "target", layout: { width: 160, height: 28 })
            Panel(key: "reveal.after", layout: { width: 180, height: 172 })
          }
        }
        Panel(key: "reveal.outer.after", layout: { width: 200, height: 300 })
      }
    }
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("post-layout-reveal", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Reveal", 320.0, 400.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const auto offset = [&surface](const std::string_view key) {
        return surface->input().scroll_offset(key).value_or(ui::Point{-1.0, -1.0}).y;
    };
    check(offset("reveal.inner") == 0.0 && offset("reveal.outer") == 0.0,
          "reveal fixture did not start unscrolled");

    // A reveal without focus still scrolls its attached target into view, with its padding.
    static_cast<void>(surface->dispatch_action("reveal.request",
                                               reveal_payload("reveal.target", false, 10.0),
                                               "test", std::nullopt, runtime::Value{}));
    static_cast<void>(surface->frame(now += 16'000'000));
    // The reveal ran after the frame's layout; the next frame must lay out its scroll instead of
    // settling on the stale layout.
    static_cast<void>(surface->frame(now += 16'000'000));
    const ui::LayoutRecord* target =
        surface->layout().find(surface->tree().find_key("reveal.target")->identity());
    const ui::LayoutRecord* inner =
        surface->layout().find(surface->tree().find_key("reveal.inner")->identity());
    check(target != nullptr && inner != nullptr && inner->viewport.has_value(),
          "reveal fixture has no layout");
    check(offset("reveal.inner") > 0.0, "a post-layout reveal never reached the layout");
    check(std::abs(target->bounds.bottom() + 10.0 - inner->viewport->bottom()) < 0.5,
          "a reveal ignored its padding");
    // The inner scroller already brought the target into the outer viewport: the outer one, which
    // must reveal where the inner scroll left the target, does not move.
    check(offset("reveal.outer") == 0.0, "an outer scroller overshot a nested reveal");
    check(!surface->input().focused_key().has_value(), "a reveal without focus took focus");

    // A focusing reveal of a target already in view focuses it without scrolling.
    const double inner_before = offset("reveal.inner");
    static_cast<void>(surface->dispatch_action("reveal.request",
                                               reveal_payload("reveal.target", true, 0.0), "test",
                                               std::nullopt, runtime::Value{}));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->input().focused_key() == std::optional<std::string_view>("reveal.target"),
          "a focusing reveal did not focus its target");
    check(offset("reveal.inner") == inner_before, "a reveal of a visible target scrolled");
}

void test_sticky_table_header_blocks_hits(const std::filesystem::path& resources) {
    std::string rows;
    for (int index = 0; index < 12; ++index) {
        rows += std::string(index == 0 ? "" : ", ") + "{ key: \"sticky.row." +
                std::to_string(index) + "\", cells: { name: \"Row " + std::to_string(index) +
                "\" } }";
    }
    const std::string source = R"(
overlay StickyTable {
  root Panel(key: "sticky.root", layout: { kind: "COLUMN", width: 320, height: 240 }) {
    Table(
      key: "sticky.table",
      columns: [{ id: "name", header: "Name", size: "FIXED", width: 200 }],
      rows: [)" + rows + R"(],
      rowHeight: 30,
      headerHeight: 32,
      layout: { kind: "SCROLL", width: 240, height: 150 }
    )
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("sticky-table", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "StickyTable", 320.0, 240.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    ui::InputOperationResult scrolled;
    check(surface->input().scroll_to("sticky.table", ui::Point{0.0, 50.0}, scrolled),
          "sticky table did not scroll");
    for (int frame = 0; frame < 2; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    const ui::RetainedNode* table = surface->tree().find_key("sticky.table");
    const ui::LayoutRecord* record =
        table != nullptr ? surface->layout().find(table->identity()) : nullptr;
    check(record != nullptr && record->viewport.has_value(), "sticky table has no viewport");
    const ui::Rect viewport = *record->viewport;
    // A row scrolled beneath the header is drawn clipped away there; it must not be hit there.
    const ui::Point under_header{viewport.x + 40.0, viewport.y + 20.0};
    bool row_beneath = false;
    for (const auto& row : table->children()) {
        const ui::LayoutRecord* row_layout = surface->layout().find(row->identity());
        row_beneath = row_beneath ||
                      (row_layout != nullptr && row_layout->bounds.y <= under_header.y &&
                       row_layout->bounds.bottom() >= under_header.y);
    }
    check(row_beneath, "sticky table fixture has no row beneath its header");
    const ui::RetainedNode* hit = surface->input().inspection_target(under_header);
    check(hit == table, "a row hidden beneath a sticky table header received the hit");
    // Below the header the rows are hit as before.
    const ui::RetainedNode* below = surface->input().inspection_target(
        ui::Point{viewport.x + 40.0, viewport.y + 32.0 + 20.0});
    bool inside_table = false;
    for (const ui::RetainedNode* current = below; current != nullptr; current = current->parent())
        inside_table = inside_table || (current != below && current == table);
    check(below != table && inside_table, "a visible table row lost its hit");
}

void test_subtargets_hit_in_presented_space(const std::filesystem::path& resources) {
    const std::string source = R"(
animation SlideIn {
  from { translateX: 200 }
  to { translateX: 0 }
  duration: 1000ms;
  easing: "linear";
  fillMode: "BOTH";
}
overlay SlidingTabs {
  state tab = "three";
  root Panel(key: "sliding.root", layout: { kind: "COLUMN", width: 640, height: 200 }) {
    Panel(key: "sliding.panel", enter: SlideIn, layout: { kind: "COLUMN", width: 300, height: 40 }) {
      Tabs(
        key: "sliding.tabs",
        tabs: [{ id: "one", label: "One" }, { id: "two", label: "Two" }, { id: "three", label: "Three" }],
        bind: tab,
        layout: { width: 300, height: 32 }
      )
    }
    Text(key: "sliding.value", text: tab)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("sliding-tabs", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "SlidingTabs", 640.0, 200.0, false);
    static_cast<void>(surface->frame(1'000'000));
    // Halfway through, the panel is drawn 100 to the right: one tab width.
    static_cast<void>(surface->frame(501'000'000));
    const ui::RetainedNode* tabs = surface->tree().find_key("sliding.tabs");
    check(tabs != nullptr, "sliding tabs fixture lost its Tabs");
    const std::vector<ui::WidgetSubtarget> targets = surface->input().subtargets(tabs->identity());
    const auto first = std::ranges::find(targets, std::string("one"), &ui::WidgetSubtarget::id);
    check(first != targets.end(), "Tabs did not project its tab subtargets");
    // IME placement, injection and spatial navigation take the presented transform from the same
    // pass that places hit regions, so the drawn bounds are where hit testing finds the node.
    const ui::LayoutRecord* tabs_layout = surface->layout().find(tabs->identity());
    const ui::Rect drawn_tabs = surface->input().presented_bounds(*tabs, tabs_layout->bounds);
    check(std::abs(drawn_tabs.x - (tabs_layout->bounds.x + 100.0)) < 2.0 &&
              surface->input().inspection_target(center(drawn_tabs)) == tabs,
          "presented bounds disagreed with where hit testing places a moving node");
    // Press where the first tab is drawn, which is where the second one is laid out.
    const ui::Point drawn{center(first->bounds).x + 100.0, center(first->bounds).y};
    std::vector<ui::SurfaceInputEvent> events;
    events.emplace_back(
        ui::PointerInputEvent{ui::Point{drawn.x + 1.0, drawn.y}, ui::PointerEventType::move, 7, 0});
    events.emplace_back(ui::PointerInputEvent{drawn, ui::PointerEventType::press, 7, 0});
    events.emplace_back(ui::PointerInputEvent{drawn, ui::PointerEventType::release, 7, 0});
    static_cast<void>(surface->input().enqueue(std::move(events)));
    static_cast<void>(surface->frame(502'000'000));
    static_cast<void>(surface->frame(503'000'000));
    const ui::RetainedNode* value = surface->tree().find_key("sliding.value");
    const auto text = value != nullptr ? value->description().properties.find("text")
                                       : ui::DescriptionNode::Properties::const_iterator{};
    const runtime::Value* selected = value != nullptr &&
                                             text != value->description().properties.end()
                                         ? text->second.value()
                                         : nullptr;
    check(selected != nullptr && selected->string() != nullptr && *selected->string() == "one",
          "a tab inside a moving panel was hit where it is laid out instead of where it is drawn");
}

void test_frame_input_prepares_are_explained(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay ChoiceFrame {
  state tab = "two";
  root Panel(key: "choice.root", layout: { kind: "COLUMN", width: 640, height: 200 }) {
    Tabs(
      key: "choice.tabs",
      tabs: [{ id: "one", label: "One" }, { id: "two", label: "Two" }],
      bind: tab,
      layout: { width: 300, height: 32 }
    )
    Text(key: "choice.value", text: tab)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("choice-frame", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "ChoiceFrame", 640.0, 200.0, false);
    static_cast<void>(surface->frame(1'000'000));
    const std::span<const ui::InputPrepareReason> first = surface->last_frame_input_prepares();
    check(!first.empty() && first.front() == ui::InputPrepareReason::rebuilt,
          "the first frame did not prepare input once for its rebuilt tree");
    static_cast<void>(surface->frame(2'000'000));
    check(surface->last_frame_input_prepares().empty() && surface->last_frame_phases().empty(),
          "a settled frame ran phases although nothing changed");

    const ui::RetainedNode* tabs = surface->tree().find_key("choice.tabs");
    const std::vector<ui::WidgetSubtarget> targets = surface->input().subtargets(tabs->identity());
    const auto first_tab = std::ranges::find(targets, std::string("one"), &ui::WidgetSubtarget::id);
    check(first_tab != targets.end(), "choice fixture did not project its tabs");
    std::vector<ui::SurfaceInputEvent> events;
    events.emplace_back(
        ui::PointerInputEvent{center(first_tab->bounds), ui::PointerEventType::press, 9, 0});
    events.emplace_back(
        ui::PointerInputEvent{center(first_tab->bounds), ui::PointerEventType::release, 9, 0});
    static_cast<void>(surface->input().enqueue(std::move(events)));
    static_cast<void>(surface->frame(3'000'000));
    const std::span<const ui::InputPrepareReason> clicked = surface->last_frame_input_prepares();
    // The click re-describes the tree once; every preparation follows a tree change.
    check(!clicked.empty() && clicked.size() <= 2U &&
              std::ranges::all_of(clicked,
                                  [](const ui::InputPrepareReason reason) {
                                      return reason == ui::InputPrepareReason::rebuilt ||
                                             reason == ui::InputPrepareReason::input_rebuild;
                                  }),
          "a state-changing click prepared input an unexplained number of times");
    // The click's frame runs the phases in their fixed order: input is drained before the build
    // it invalidates, layout follows every build, and semantics and render close the frame once.
    const std::span<const ui::FramePhase> phases = surface->last_frame_phases();
    const auto position = [&phases](const ui::FramePhase phase) {
        return static_cast<std::size_t>(std::ranges::find(phases, phase) - phases.begin());
    };
    check(!phases.empty() && phases.front() == ui::FramePhase::input &&
              phases.back() == ui::FramePhase::render &&
              position(ui::FramePhase::input) < position(ui::FramePhase::build) &&
              position(ui::FramePhase::build) < position(ui::FramePhase::layout) &&
              position(ui::FramePhase::layout) < position(ui::FramePhase::post_layout) &&
              position(ui::FramePhase::post_layout) < position(ui::FramePhase::semantics) &&
              std::ranges::count(phases, ui::FramePhase::render) == 1,
          "a click's frame did not run its phases in order");
    static_cast<void>(surface->frame(4'000'000));
    static_cast<void>(surface->frame(5'000'000));
    check(surface->last_frame_input_prepares().empty(),
          "input preparation continued after the click settled");
}

void test_item_grid_header_clip(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay GroupedGrid {
  root Panel(key: "grid.root", layout: { kind: "COLUMN", width: 640, height: 300 }) {
    ItemGrid(
      key: "grid.items",
      entries: [
        { key: "group", kind: "HEADER", label: "Group" },
        { key: "a", kind: "ITEM", label: "A" },
        { key: "b", kind: "ITEM", label: "B" }
      ],
      gap: 10,
      layout: { width: 400, height: 240 }
    )
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("grouped-grid", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "GroupedGrid", 640.0, 300.0, false);
    static_cast<void>(surface->frame(1'000'000));
    const ui::RetainedNode* grid = surface->tree().find_key("grid.items");
    const ui::LayoutRecord* record =
        grid != nullptr ? surface->layout().find(grid->identity()) : nullptr;
    check(record != nullptr, "grouped grid fixture was not laid out");
    const ui::Rect viewport = record->viewport.value_or(record->bounds);
    // The descendant clip strip is the header band's own height, not a constant or the band's
    // virtual extent (which adds the grid gap).
    const bool clipped_at_header = std::ranges::any_of(
        surface->render_commands().commands(), [&viewport](const ui::RenderCommand& command) {
            const auto* clip = std::get_if<ui::ClipPushRenderCommand>(&command);
            return clip != nullptr && std::abs(clip->rect.y - (viewport.y + 26.0)) < 0.01 &&
                   std::abs(clip->rect.height - (viewport.height - 26.0)) < 0.01;
        });
    check(clipped_at_header, "item grid did not clip its descendants at the header band height");
}

void test_queued_batch_sees_rebuilt_tree(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay BatchedReveal {
  state open = false;
  state draft = "";
  root Panel(key: "batch.root", layout: { kind: "COLUMN", width: 300, height: 200, gap: 8 }) {
    Button(key: "batch.open", label: "Open", onClick: action("state.set", name: "open", value: true))
    if open {
      TextBox(key: "batch.field", bind: draft, layout: { width: 160, height: 28 })
    }
    Text(key: "batch.draft", text: draft)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("batched-reveal", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "BatchedReveal", 300.0, 200.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    static_cast<void>(surface->dispatch_action(
        "focus.request", object({{"key", runtime::Value(runtime::KeyValue{"batch.open"})}}),
        "test", std::nullopt, runtime::Value{}));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->input().focused_key() == std::optional<std::string_view>("batch.open") &&
              surface->tree().find_key("batch.field") == nullptr,
          "batched reveal fixture did not start focused on its closed trigger");

    // One platform batch: Enter reveals a field, Tab moves into it, and the text lands there.
    // Tab and the text must be routed against the tree the Enter rebuilt.
    std::vector<ui::SurfaceInputEvent> events;
    events.emplace_back(ui::KeyInputEvent{"enter"});
    events.emplace_back(ui::KeyInputEvent{"tab"});
    events.emplace_back(ui::TextInputEvent{"typed"});
    static_cast<void>(surface->input().enqueue(std::move(events)));
    for (int frame = 0; frame < 2; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->input().focused_key() == std::optional<std::string_view>("batch.field"),
          "Tab after the key revealing a field was routed against the tree without it");
    const ui::RetainedNode* draft = surface->tree().find_key("batch.draft");
    const auto text = draft != nullptr ? draft->description().properties.find("text")
                                       : ui::DescriptionNode::Properties::const_iterator{};
    const runtime::Value* value = draft != nullptr &&
                                          text != draft->description().properties.end()
                                      ? text->second.value()
                                      : nullptr;
    check(value != nullptr && value->string() != nullptr && *value->string() == "typed",
          "text after the key revealing a field did not reach it");
}

void test_hover_follows_content_under_still_pointer(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay HoverScroll {
  state open = false;
  root Panel(key: "hover.root", layout: { kind: "COLUMN", width: 300, height: 300 }) {
    Scroll(key: "hover.scroll", vertical: true, horizontal: false, layout: { width: 200, height: 120 }) {
      Panel(key: "hover.content", layout: { kind: "COLUMN", width: 200, height: 400 }) {
        Panel(key: "hover.row.0", layout: { width: 200, height: 40 })
        Panel(key: "hover.row.1", layout: { width: 200, height: 40 })
        Panel(key: "hover.row.2", layout: { width: 200, height: 40 })
        Panel(key: "hover.row.3", layout: { width: 200, height: 280 })
      }
    }
    if open {
      Panel(key: "hover.popup", layout: { kind: "PORTAL", width: 200, height: 120, anchorPoint: { x: 0, y: 0 }, anchorGap: 0, anchorSide: "BOTTOM" })
    }
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("hover-scroll", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "HoverScroll", 300.0, 300.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const auto hovered = [&surface](const std::string_view key) {
        const ui::RetainedNode* node = surface->tree().find_key(key);
        return node != nullptr && surface->input().hovered(node->identity());
    };
    const ui::Point still = center(
        surface->layout().find(surface->tree().find_key("hover.row.0")->identity())->bounds);
    std::vector<ui::SurfaceInputEvent> events;
    events.emplace_back(ui::PointerInputEvent{still, ui::PointerEventType::move, 4, 0});
    static_cast<void>(surface->input().enqueue(std::move(events)));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(hovered("hover.row.0") && !hovered("hover.row.2"), "hover fixture did not hover row 0");

    // Content scrolled beneath a pointer that does not move: the row now under it is hovered.
    ui::InputOperationResult scrolled;
    check(surface->input().scroll_to("hover.scroll", ui::Point{0.0, 80.0}, scrolled),
          "hover fixture did not scroll");
    for (int frame = 0; frame < 2; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(hovered("hover.row.2") && !hovered("hover.row.0"),
          "hover stayed on content that scrolled away from a still pointer");

    // A popup opening under the still pointer takes the hover.
    static_cast<void>(surface->dispatch_action(
        "state.set", object({{"name", runtime::Value("open")}, {"value", runtime::Value(true)}}),
        "test", std::string("hover.root"), runtime::Value{}));
    for (int frame = 0; frame < 2; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->tree().find_key("hover.popup") != nullptr,
          "hover fixture did not open its popup");
    check(hovered("hover.popup") && !hovered("hover.row.2"),
          "a popup opened under a still pointer did not take its hover");
}

void test_pointer_cursor_follows_what_is_hovered(const std::filesystem::path& resources) {
    const std::string source = R"(
style Crosshair { cursor: "CROSSHAIR"; }
overlay Cursors {
  root Panel(key: "cursor.root", layout: { kind: "COLUMN", width: 300, height: 300 }) {
    TextBox(key: "cursor.edit", text: "hello", layout: { width: 200, height: 30 })
    Panel(key: "cursor.custom", style: Crosshair, layout: { kind: "COLUMN", width: 200, height: 60 }) {
      Panel(key: "cursor.inner", layout: { width: 200, height: 30 })
    }
    Panel(key: "cursor.plain", layout: { width: 200, height: 30 })
    SplitPane(key: "cursor.split", layout: { width: 200, height: 60 }) {
      Panel(key: "cursor.split.a")
      Panel(key: "cursor.split.b")
    }
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("pointer-cursors", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Cursors", 300.0, 300.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const auto cursor_over = [&](const std::string_view key) {
        const ui::RetainedNode* node = surface->tree().find_key(key);
        check(node != nullptr, "pointer cursor fixture is missing a node");
        std::vector<ui::SurfaceInputEvent> events;
        events.emplace_back(ui::PointerInputEvent{
            center(surface->layout().find(node->identity())->bounds), ui::PointerEventType::move,
            4, 0});
        static_cast<void>(surface->input().enqueue(std::move(events)));
        static_cast<void>(surface->frame(now += 16'000'000));
        return surface->input().cursor();
    };
    check(cursor_over("cursor.edit") == ui::PointerCursor::text,
          "an editable text box did not show the text cursor");
    check(cursor_over("cursor.inner") == ui::PointerCursor::crosshair,
          "a child did not inherit its ancestor's authored cursor");
    check(cursor_over("cursor.plain") == ui::PointerCursor::standard,
          "a plain panel did not show the default cursor");
    check(cursor_over("cursor.split.divider") == ui::PointerCursor::resize_ew,
          "a horizontal split divider did not show the east-west resize cursor");
    check(ui::pointer_cursor_from_name(ui::pointer_cursor_name(ui::PointerCursor::resize_nwse)) ==
              ui::PointerCursor::resize_nwse,
          "pointer cursor names do not round-trip");
}

void test_text_decoration_and_named_weight(const std::filesystem::path& resources) {
    const std::string source = R"(
style Strong { fontWeight: "BOLD"; fontStyle: "ITALIC"; textDecoration: "UNDERLINE"; textDecorationColor: #FF0000FF; }
overlay Decorated {
  root Panel(key: "decor.root", layout: { kind: "COLUMN", width: 300, height: 120 }) {
    Text(key: "decor.strong", text: "Strong words", style: Strong)
    Text(key: "decor.plain", text: "Plain words")
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("text-decoration", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Decorated", 300.0, 120.0);
    static_cast<void>(surface->frame(1'000'000));
    std::size_t red_lines = 0U;
    std::uint32_t strong_flags = 0U;
    for (const ui::RenderCommand& command : surface->render_commands().commands()) {
        if (const auto* rect = std::get_if<ui::SolidRectRenderCommand>(&command)) {
            const runtime::ColorValue* color = rect->fill.color();
            if (color != nullptr && color->red == 255U && color->green == 0U &&
                color->blue == 0U && rect->bounds.width > 20.0 && rect->bounds.height <= 2.0) {
                ++red_lines;
            }
        }
        if (const auto* run = std::get_if<ui::TextRunRenderCommand>(&command);
            run != nullptr && !run->glyphs.empty() && strong_flags == 0U) {
            strong_flags = run->glyphs.front().font_style_flags;
        }
    }
    check(red_lines == 1U, "an underlined text did not draw exactly one underline in its color");
    check(strong_flags == (font::font_style_bold | font::font_style_italic),
          "named BOLD weight and ITALIC style did not resolve to bold italic glyphs");
}

void test_per_corner_radius_rounds_each_corner(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Corners {
  root Panel(key: "corners.root", layout: { kind: "COLUMN", width: 300, height: 120 }) {
    Panel(key: "corners.tab", style: style(radius: { top: 12, bottomRight: 4 }, background: #336699FF, clip: true), layout: { width: 120, height: 40 }) {
      Panel(key: "corners.inner", style: style(background: #FFFFFFFF), layout: { width: 120, height: 40 })
    }
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("per-corner-radius", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Corners", 300.0, 120.0);
    static_cast<void>(surface->frame(1'000'000));
    const ui::CornerRadii expected{12.0, 12.0, 4.0, 0.0};
    bool background = false;
    bool clip = false;
    for (const ui::RenderCommand& command : surface->render_commands().commands()) {
        if (const auto* rect = std::get_if<ui::RoundedRectRenderCommand>(&command);
            rect != nullptr && rect->fill.color() != nullptr && rect->fill.color()->blue == 0x99U) {
            background = rect->radii == expected;
        }
        if (const auto* pushed = std::get_if<ui::ClipPushRenderCommand>(&command);
            pushed != nullptr && pushed->radii == expected) {
            clip = true;
        }
    }
    check(background, "a per-corner radius did not round the background's corners separately");
    check(clip, "a clipping panel did not clip its children to its per-corner radius");
}

void test_text_list_and_math_helpers(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Helpers {
  state names = ["ada", "grace", "linus"];
  root Panel(key: "helpers.root", layout: { kind: "COLUMN", width: 300, height: 300 }) {
    Text(key: "helpers.split", text: join(split("a,b,,c", ","), "|"))
    Text(key: "helpers.replace", text: replace("one two two", "two", "2"))
    Text(key: "helpers.slice", text: slice("hello world", -5))
    Text(key: "helpers.index", text: formatNumber(indexOf("hello", "ll"), 0))
    Text(key: "helpers.pad", text: padStart("7", 3, "0") + padEnd("x", 3, "-"))
    Text(key: "helpers.first", text: first(names) + ":" + last(names))
    Text(key: "helpers.reverse", text: join(reverse(names), " "))
    Text(key: "helpers.sum", text: formatNumber(sum(range(1, 5)), 0) + "/" + formatNumber(sum(names, n -> length(n)), 0))
    Text(key: "helpers.find", text: find(names, n -> startsWith(n, "g")))
    Text(key: "helpers.math", text: formatNumber(sqrt(16) + pow(2, 10), 0))
    Text(key: "helpers.list", text: join(slice(range(0, 10, 3), 1, 3), ","))
    Text(key: "helpers.date", text: formatDate(1709287205123, "EEE, d MMM yyyy 'at' HH:mm:ss.SSS"))
    Text(key: "helpers.date.offset", text: formatDate(1709287205123, "yyyy-MM-dd h:mm a", -480))
    Text(key: "helpers.date.early", text: formatDate(-86400000, "EEEE yyyy-MM-dd"))
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("expression-helpers", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Helpers", 300.0, 300.0);
    static_cast<void>(surface->frame(1'000'000));
    const auto text = [&](const std::string_view key) {
        const ui::RetainedNode* node = surface->tree().find_key(key);
        check(node != nullptr, "helper fixture is missing a text node");
        const auto found = node->description().properties.find("text");
        const runtime::Value* value =
            found != node->description().properties.end() ? found->second.value() : nullptr;
        return value != nullptr && value->string() != nullptr ? *value->string() : std::string{};
    };
    check(text("helpers.split") == "a|b||c", "split did not keep empty fields between separators");
    check(text("helpers.replace") == "one 2 2", "replace did not replace every occurrence");
    check(text("helpers.slice") == "world", "slice did not count a negative start from the end");
    check(text("helpers.index") == "2", "indexOf did not find a substring");
    check(text("helpers.pad") == "007x--", "padStart or padEnd did not pad to the width");
    check(text("helpers.first") == "ada:linus", "first or last did not return an end of a list");
    check(text("helpers.reverse") == "linus grace ada", "reverse did not reverse a list");
    check(text("helpers.sum") == "10/13", "sum did not add numbers or selected values");
    check(text("helpers.find") == "grace", "find did not return the first matching item");
    check(text("helpers.math") == "1028", "sqrt or pow computed the wrong value");
    check(text("helpers.list") == "3,6", "range with a step or list slice was wrong");
    check(text("helpers.date") == "Fri, 1 Mar 2024 at 10:00:05.123",
          "formatDate did not format a UTC timestamp");
    check(text("helpers.date.offset") == "2024-03-01 2:00 AM",
          "formatDate did not apply its UTC offset or 12-hour clock");
    check(text("helpers.date.early") == "Wednesday 1969-12-31",
          "formatDate did not format a timestamp before 1970");
}

void test_number_bytes_duration_and_relative_time_formats(const std::filesystem::path& resources) {
    const auto number = [](const double value, const auto& configure) {
        runtime::NumberFormat format;
        configure(format);
        return runtime::format_number(value, format);
    };
    const auto plain = [](runtime::NumberFormat&) {};
    check(number(1234.5, plain) == "1234.50", "formatNumber's default changed from two decimals");
    check(number(1234567.891, [](auto& f) { f.grouping = true; }) == "1,234,567.89",
          "formatNumber did not group thousands");
    check(number(1234.5, [](auto& f) { f.currency = "USD"; }) == "$1,234.50",
          "formatNumber did not write US dollars");
    check(number(-1234.5, [](auto& f) { f.currency = "USD"; }) == "-$1,234.50",
          "formatNumber did not put the sign before the currency symbol");
    check(number(1234.5, [](auto& f) { f.currency = "JPY"; }) == "\xC2\xA5"
                                                                 "1,235",
          "formatNumber did not use the yen's zero minor digits");
    check(number(1234.5, [](auto& f) { f.currency = "CHF"; }) == "CHF 1,234.50",
          "formatNumber did not write a currency without a symbol as its code");
    check(number(1234.5,
                 [](auto& f) {
                     f.currency = "EUR";
                     f.group_separator = ".";
                     f.decimal_separator = ",";
                 }) == "\xE2\x82\xAC"
                       "1.234,50",
          "formatNumber did not use the authored separators");
    check(number(1234.0, [](auto& f) { f.compact = true; }) == "1.2K" &&
              number(3400000.0, [](auto& f) { f.compact = true; }) == "3.4M" &&
              number(12500.0, [](auto& f) { f.compact = true; }) == "13K" &&
              number(999950.0, [](auto& f) { f.compact = true; }) == "1M" &&
              number(2.0e12, [](auto& f) { f.compact = true; }) == "2T" &&
              number(950.0, [](auto& f) { f.compact = true; }) == "950",
          "formatNumber's compact form did not scale by thousands");
    check(number(1500.0,
                 [](auto& f) {
                     f.compact = true;
                     f.decimals = 2;
                 }) == "1.50K",
          "formatNumber's compact form ignored authored decimals");
    check(number(0.256, [](auto& f) { f.percent = true; }) == "26%" && number(0.256,
                                                                              [](auto& f) {
                                                                                  f.percent = true;
                                                                                  f.decimals = 1;
                                                                              }) == "25.6%",
          "formatNumber did not write a percentage");
    check(number(5.0, [](auto& f) { f.sign_display = "always"; }) == "+5.00" &&
              number(0.0, [](auto& f) { f.sign_display = "exceptZero"; }) == "0.00" &&
              number(-0.001, plain) == "0.00" &&
              number(-5.0, [](auto& f) { f.sign_display = "never"; }) == "5.00",
          "formatNumber did not follow its sign display");
    check(runtime::format_bytes(512.0, std::nullopt, false) == "512 B" &&
              runtime::format_bytes(1500000.0, std::nullopt, false) == "1.5 MB" &&
              runtime::format_bytes(2000.0, std::nullopt, false) == "2 KB" &&
              runtime::format_bytes(1536.0, std::nullopt, true) == "1.5 KiB" &&
              runtime::format_bytes(999960.0, std::nullopt, false) == "1 MB" &&
              runtime::format_bytes(3.25e9, 2, false) == "3.25 GB",
          "formatBytes did not pick the largest unit");
    constexpr double minute = 60'000.0;
    constexpr double day = 86'400'000.0;
    const double now = 1'700'000'000'000.0;
    check(runtime::time_ago(now - 3000.0, now, false) == "just now" &&
              runtime::time_ago(now - 42'000.0, now, false) == "42 seconds ago" &&
              runtime::time_ago(now - minute, now, false) == "1 minute ago" &&
              runtime::time_ago(now - 3.5 * minute, now, false) == "3 minutes ago" &&
              runtime::time_ago(now + 2.0 * day, now, false) == "in 2 days" &&
              runtime::time_ago(now - 9.0 * day, now, false) == "1 week ago" &&
              runtime::time_ago(now - 45.0 * day, now, false) == "1 month ago" &&
              runtime::time_ago(now - 800.0 * day, now, false) == "2 years ago" &&
              runtime::time_ago(now - 3.0 * 3'600'000.0, now, true) == "3h ago" &&
              runtime::time_ago(now + 2.0 * day, now, true) == "in 2d",
          "timeAgo did not describe the distance in its largest whole unit");
    check(runtime::format_duration(3'909'000.0, "clock") == "1:05:09" &&
              runtime::format_duration(309'000.0, "clock") == "5:09" &&
              runtime::format_duration(3'909'000.0, "short") == "1h 5m" &&
              runtime::format_duration(309'000.0, "short") == "5m 9s" &&
              runtime::format_duration(350.0, "short") == "350ms" &&
              runtime::format_duration(3'661'000.0, "long") == "1 hour 1 minute" &&
              runtime::format_duration(-65'000.0, "clock") == "-1:05",
          "formatDuration did not write its style");

    // The same formats reached through the language, positionally and by name.
    const std::string source = R"(
overlay Formats {
  state now = 1700000000000;
  state wall = 0;
  root Panel(key: "formats.root", layout: { kind: "COLUMN", width: 300, height: 300 }) {
    Text(key: "formats.money", text: formatNumber(1234.5, currency: "USD"))
    Text(key: "formats.compact", text: formatNumber(3400000, compact: true))
    Text(key: "formats.grouped", text: formatNumber(1234567, 0, true))
    Text(key: "formats.percent", text: formatNumber(0.5, percent: true, signDisplay: "always"))
    Text(key: "formats.bytes", text: formatBytes(1500000))
    Text(key: "formats.ago", text: timeAgo(now - 180000, now))
    Text(key: "formats.duration", text: formatDuration(3909000) + " " + formatDuration(3909000, "short"))
    Panel(key: "formats.clock", timers: [{ id: "clock", every: 1ms, action: then(action("clock.now"), action("state.setFromEvent", name: "wall", field: "now")) }])
    Text(key: "formats.wall", text: wall > 1600000000000 ? "read" : "unread")
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("formats", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Formats", 300.0, 300.0);
    static_cast<void>(surface->frame(1'000'000));
    const auto text = [&](const std::string_view key) {
        const ui::RetainedNode* node = surface->tree().find_key(key);
        check(node != nullptr, "format fixture is missing a text node");
        const auto found = node->description().properties.find("text");
        const runtime::Value* value =
            found != node->description().properties.end() ? found->second.value() : nullptr;
        return value != nullptr && value->string() != nullptr ? *value->string() : std::string{};
    };
    check(text("formats.money") == "$1,234.50", "formatNumber's currency was not reachable");
    check(text("formats.compact") == "3.4M", "formatNumber's compact form was not reachable");
    check(text("formats.grouped") == "1,234,567", "formatNumber's grouping was not positional");
    check(text("formats.percent") == "+50%", "formatNumber's percent and sign were not reachable");
    check(text("formats.bytes") == "1.5 MB", "formatBytes was not reachable");
    check(text("formats.ago") == "3 minutes ago", "timeAgo was not reachable");
    check(text("formats.duration") == "1:05:09 1h 5m", "formatDuration was not reachable");
    check(text("formats.wall") == "unread", "the wall clock was read before its timer fired");
    static_cast<void>(surface->frame(3'000'000));
    static_cast<void>(surface->frame(6'000'000));
    check(text("formats.wall") == "read", "clock.now did not answer then() with the wall clock");
}

void test_paint_rotate_spins_a_gradient_border(const std::filesystem::path& resources) {
    const std::string source = R"(
animation BorderSpin { duration: 1000ms; easing: "linear"; repeat: "forever"; from { paintRotate: 0 } to { paintRotate: 360 } }
overlay Spin {
  root Panel(key: "spin.root", layout: { kind: "COLUMN", width: 300, height: 200 }) {
    Panel(key: "spin.card", enter: BorderSpin, style: style(width: 200, height: 100, radius: 16, background: #101010FF,
      border: { width: 3, paint: { kind: "conic", stops: [ { color: #FF0000FF }, { color: #0000FFFF } ] } }))
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("paint-rotate", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Spin", 300.0, 200.0, false);
    const auto rotation = [&]() -> std::optional<double> {
        for (const ui::RenderCommand& command : surface->render_commands().commands()) {
            if (const auto* rect = std::get_if<ui::RoundedRectRenderCommand>(&command);
                rect != nullptr && rect->border.has_value() && rect->border->paint.is_gradient()) {
                return rect->border->paint.gradient()->rotation_degrees;
            }
            if (const auto* border = std::get_if<ui::BorderRenderCommand>(&command);
                border != nullptr && border->border.paint.is_gradient()) {
                return border->border.paint.gradient()->rotation_degrees;
            }
        }
        return std::nullopt;
    };
    static_cast<void>(surface->frame(1'000'000));
    const std::optional<double> first = rotation();
    static_cast<void>(surface->frame(251'000'000));
    const std::optional<double> second = rotation();
    static_cast<void>(surface->frame(1'501'000'000));
    const std::optional<double> third = rotation();
    check(first.has_value() && second.has_value() && third.has_value(),
          "a gradient border was not drawn with its paint");
    check(std::abs(*second - *first - 90.0) < 2.0,
          "paintRotate did not turn the border's gradient as the animation ran");
    check(std::abs(*third - 180.0) < 2.0, "a repeating paintRotate animation did not loop");
}

void test_code_view_highlights_numbers_and_copies(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Code {
  state wrapped = false;
  root Panel(key: "code.root", layout: { kind: "COLUMN", width: 600, height: 600 }) {
    CodeView(key: "code.view", text: "fn main() {\n\tlet x = 42; // answer\n}\n", language: "rust",
             highlightLines: [2], wrap: wrapped, layout: { height: 200 },
             style: style(syntax: { comment: #00FF00FF }))
    Markdown(key: "code.md", text: "Before\n\n```python\nprint(\"hi\")\n```")
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("code-view", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Code", 600.0, 600.0, false);
    std::int64_t now = 1'000'000;
    for (int frame = 0; frame < 4; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    const auto node = [&](const std::string_view key) {
        const ui::RetainedNode* found = surface->tree().find_key(key);
        check(found != nullptr, "code view fixture is missing " + std::string(key));
        return found;
    };
    const auto property = [](const ui::RetainedNode* found, const std::string_view name) {
        const auto value = found->description().properties.find(name);
        return value != found->description().properties.end() ? value->second.value() : nullptr;
    };
    // Three rows: line 1, the highlighted line 2 alone, and line 3; the final newline ends line
    // 3 rather than starting a fourth, and the tab became spaces.
    const ui::RetainedNode* second = node("code.view.r1.t");
    const runtime::Value* second_text = property(second, "text");
    check(second_text != nullptr && second_text->string() != nullptr &&
              *second_text->string() == "    let x = 42; // answer",
          "a CodeView row did not hold its line with its tab expanded");
    check(surface->tree().find_key("code.view.r3.t") == nullptr,
          "a final newline started a line of its own");
    const runtime::Value* row_background = property(node("code.view.r1"), "background");
    check(row_background != nullptr && row_background->color() != nullptr,
          "a highlighted line did not draw its band");
    const runtime::Value* numbers = property(node("code.view.r0.n"), "text");
    check(numbers != nullptr && numbers->string() != nullptr && *numbers->string() == "1",
          "the gutter did not number its line");
    // Tokens are coloured spans: the theme's keyword colour, and the author's comment colour.
    const runtime::Value* spans = property(second, "spans");
    check(spans != nullptr && spans->list() != nullptr, "a CodeView row had no spans");
    std::optional<runtime::ColorValue> keyword;
    std::optional<runtime::ColorValue> comment;
    for (const runtime::Value& span : spans->list()->values) {
        const runtime::Value* text = span.field("text");
        const runtime::Value* color = span.field("color");
        if (text == nullptr || text->string() == nullptr || color == nullptr ||
            color->color() == nullptr)
            continue;
        if (*text->string() == "let")
            keyword = *color->color();
        if (text->string()->starts_with("//"))
            comment = *color->color();
    }
    check(keyword.has_value() && comment.has_value(),
          "a CodeView's spans did not resolve their colours");
    check(*comment == runtime::ColorValue{0U, 255U, 0U, 255U},
          "an authored syntax colour did not reach its tokens");
    check(*keyword != *comment, "keywords and comments shared one colour");

    // Selecting all of the code and copying it gives the text back, across its rows.
    const ui::Rect code_bounds = surface->layout().find(second->identity())->bounds;
    const ui::Point inside{code_bounds.x + 10.0, code_bounds.y + code_bounds.height * 0.5};
    std::vector<ui::SurfaceInputEvent> copy;
    copy.emplace_back(ui::PointerInputEvent{inside, ui::PointerEventType::press, 5, 0});
    copy.emplace_back(ui::PointerInputEvent{inside, ui::PointerEventType::release, 5, 0});
    copy.emplace_back(ui::KeyInputEvent{"a", ui::KeyModifiers{false, true, false, false}});
    copy.emplace_back(ui::KeyInputEvent{"c", ui::KeyModifiers{false, true, false, false}});
    static_cast<void>(surface->input().enqueue(std::move(copy)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    const std::optional<std::string_view> copied = surface->input().clipboard_text();
    check(copied.has_value() && *copied == "fn main() {\n    let x = 42; // answer\n}",
          "copying a CodeView's selection did not give back its lines: [" +
              std::string(copied.value_or("<none>")) + "]");

    // A fenced block in Markdown is a CodeView in its fence's language.
    const ui::RetainedNode* fenced = node("code.md.1");
    check(fenced->description().type == "CodeView",
          "a fenced Markdown block was not drawn by a CodeView");
    const runtime::Value* fenced_language = property(fenced, "language");
    check(fenced_language != nullptr && fenced_language->string() != nullptr &&
              *fenced_language->string() == "python",
          "a fenced block lost its fence's language");
}

// Four thousand lines of equal length, generated the same way in Strata and here.
constexpr std::string_view long_code_lines =
    R"(join(map(range(1, 4001), n -> "let row" + (n < 10 ? "000" : n < 100 ? "00" : n < 1000 ? "0" : "") + formatNumber(n, 0) + " = " + formatNumber(n * 7 % 1000 + 1000, 0) + ";"), "\n"))";

[[nodiscard]] std::string long_code_text() {
    std::string result;
    for (int n = 1; n <= 4000; ++n) {
        std::string number = std::to_string(n);
        number.insert(0U, 4U - number.size(), '0');
        if (n > 1)
            result.push_back('\n');
        result += "let row" + number + " = " + std::to_string(n * 7 % 1000 + 1000) + ";";
    }
    return result;
}

/** Offset in `text` where its 1-based `line` starts. */
[[nodiscard]] std::size_t line_start(const std::string_view text, const std::size_t line) {
    std::size_t at = 0U;
    for (std::size_t current = 1U; current < line; ++current)
        at = text.find('\n', at) + 1U;
    return at;
}

/** How many of a code view's row texts are realized. */
[[nodiscard]] std::size_t realized_code_rows(const ui::Surface& surface, const std::string& key) {
    std::size_t count = 0U;
    const std::string prefix = key + ".r";
    const auto visit = [&](const auto& self, const ui::RetainedNode& node) -> void {
        const std::optional<std::string>& own = node.description().key;
        if (own.has_value() && own->starts_with(prefix) && own->ends_with(".t"))
            ++count;
        for (const auto& child : node.children())
            self(self, *child);
    };
    if (surface.tree().root() != nullptr)
        visit(visit, *surface.tree().root());
    return count;
}

/**
 * A code view with a height of its own realizes only the rows near its viewport, yet selecting
 * all of it copies the whole file, a shift-click selection reaches a row thousands of lines away
 * after the row it started in has gone, a drag past its edge scrolls and selects on, and the line
 * numbers beside the text never join a copy.
 */
void test_code_view_selection_spans_unrealized_rows(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay LongCode {
  root Panel(key: "long.root", layout: { kind: "COLUMN", width: 600, height: 400 }) {
    CodeView(key: "long.code", text: )" +
                               std::string(long_code_lines) +
                               R"(, language: "rust", layout: { height: 300 })
  }
}
)";
    const std::string expected = long_code_text();
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("long-code", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "LongCode", 600.0, 400.0);
    std::int64_t now = 1'000'000;
    const auto frames = [&](const int count) {
        for (int frame = 0; frame < count; ++frame)
            static_cast<void>(surface->frame(now += 16'000'000));
    };
    const auto send = [&](std::vector<ui::SurfaceInputEvent> events) {
        static_cast<void>(surface->input().enqueue(std::move(events)));
        frames(2);
    };
    const ui::KeyModifiers command{false, true, false, false};
    const ui::KeyModifiers shift{true, false, false, false};
    const auto copied = [&]() {
        send({ui::KeyInputEvent{"c", command}});
        return std::string(surface->input().clipboard_text().value_or("<none>"));
    };
    const auto row = [&](const std::size_t first) -> const ui::LayoutRecord* {
        const ui::RetainedNode* node =
            surface->tree().find_key("long.code.r" + std::to_string(first) + ".t");
        return node != nullptr ? surface->layout().find(node->identity()) : nullptr;
    };
    // The start of a line, as a point in the realized row holding it (rows hold 80 lines).
    const auto line_point = [&](const std::size_t line) -> std::optional<ui::Point> {
        const std::size_t first = (line - 1U) / 80U * 80U;
        const ui::LayoutRecord* record = row(first);
        if (record == nullptr)
            return std::nullopt;
        const double height = record->content_bounds.height / 80.0;
        return ui::Point{record->content_bounds.x + 1.0,
                         record->content_bounds.y +
                             (static_cast<double>(line - 1U - first) + 0.5) * height};
    };
    frames(4);
    check(realized_code_rows(*surface, "long.code") <= 4U,
          "a code view with a height of its own realized more than the rows near its viewport");

    // Select all and copy: the whole file, exactly, though only a few rows are realized.
    const std::optional<ui::Point> first_line = line_point(1U);
    check(first_line.has_value(), "the code view's first row was not realized");
    send({ui::PointerInputEvent{*first_line, ui::PointerEventType::press, 5, 0},
          ui::PointerInputEvent{*first_line, ui::PointerEventType::release, 5, 0},
          ui::KeyInputEvent{"a", command}});
    const std::string all = copied();
    check(all == expected, "select-all in a virtualized code view did not copy the whole file (" +
                               std::to_string(all.size()) + " of " +
                               std::to_string(expected.size()) + " bytes)");

    // Click at line 10, scroll to line 3000, shift-click it and extend to its end.
    const std::optional<ui::Point> tenth = line_point(10U);
    check(tenth.has_value(), "line 10 was not realized");
    send({ui::PointerInputEvent{*tenth, ui::PointerEventType::press, 5, 0},
          ui::PointerInputEvent{*tenth, ui::PointerEventType::release, 5, 0}});
    const double line_height = row(0U)->content_bounds.height / 80.0;
    ui::InputOperationResult scrolled;
    check(surface->input().scroll_to("long.code.scroll", ui::Point{0.0, 2995.0 * line_height},
                                     scrolled),
          "the code view did not scroll");
    frames(4);
    const std::optional<ui::Point> far = line_point(3000U);
    check(far.has_value(), "line 3000 was not realized after scrolling to it");
    send({ui::PointerInputEvent{*far, ui::PointerEventType::press, 5, 0, shift},
          ui::PointerInputEvent{*far, ui::PointerEventType::release, 5, 0, shift},
          ui::KeyInputEvent{"end", shift}});
    check(surface->tree().find_key("long.code.r0.t") == nullptr,
          "the row the selection started in was still realized far away from it");
    const std::size_t from = line_start(expected, 10U);
    const std::size_t to = line_start(expected, 3001U) - 1U;
    const std::string span = copied();
    check(span == expected.substr(from, to - from),
          "a selection from line 10 to line 3000 did not copy exactly those lines (" +
              std::to_string(span.size()) + " of " + std::to_string(to - from) + " bytes)");
    check(span.find("2999\n") == std::string::npos && span.find(" 3000") == std::string::npos,
          "line numbers joined a copied selection");

    // Shift+Down past the end of a row moves into the next one.
    send({ui::KeyInputEvent{"down", shift}, ui::KeyInputEvent{"down", shift}});
    const std::string longer = copied();
    check(longer == expected.substr(from, line_start(expected, 3003U) - 1U - from),
          "extending a selection down by lines did not take the next lines");

    // Press at line 10 and drag below the view: it scrolls, and the selection follows on into
    // rows that were not realized when the drag began.
    check(surface->input().scroll_to("long.code.scroll", ui::Point{0.0, 0.0}, scrolled),
          "the code view did not scroll back");
    frames(4);
    const std::optional<ui::Point> start = line_point(10U);
    check(start.has_value(), "line 10 was not realized again");
    const ui::LayoutRecord* list =
        surface->layout().find(surface->tree().find_key("long.code.scroll")->identity());
    const ui::Point below{start->x + 40.0, list->viewport->bottom() + 200.0};
    send({ui::PointerInputEvent{*start, ui::PointerEventType::press, 5, 0},
          ui::PointerInputEvent{ui::Point{start->x, start->y + 30.0}, ui::PointerEventType::move, 5,
                                0},
          ui::PointerInputEvent{below, ui::PointerEventType::move, 5, 0}});
    frames(90);
    send({ui::PointerInputEvent{below, ui::PointerEventType::release, 5, 0}});
    const ui::LayoutRecord* moved =
        surface->layout().find(surface->tree().find_key("long.code.scroll")->identity());
    check(moved != nullptr && moved->scroll_offset.y > 200.0 * line_height,
          "dragging a selection below a code view did not scroll it");
    const std::string dragged = copied();
    const std::size_t dragged_lines =
        static_cast<std::size_t>(std::ranges::count(dragged, '\n')) + 1U;
    check(expected.compare(from, dragged.size(), dragged) == 0 && dragged_lines > 200U,
          "a drag scrolled past the realized rows did not select on from line 10 (" +
              std::to_string(dragged_lines) + " lines)");
}

/**
 * A code view sized to its content inside a scrolling page is as tall as all its lines, so the
 * page scrolls over all of it, but realizes only the rows the page shows; its height holds still
 * as rows are measured, and select-all still copies the whole file.
 */
void test_content_sized_code_view_realizes_what_the_page_shows(
    const std::filesystem::path& resources) {
    const std::string source = R"(
overlay LongPage {
  root Panel(key: "page.root", layout: { kind: "COLUMN", width: 600, height: 400 }) {
    Scroll(key: "page.scroll", vertical: true, horizontal: false, layout: { width: 600, height: 400 }) {
      Panel(key: "page.content", layout: { kind: "COLUMN", width: 580 }) {
        Text(key: "page.intro", text: "Before")
        CodeView(key: "page.code", text: )" +
                               std::string(long_code_lines) + R"(, language: "rust")
        Text(key: "page.after", text: "After")
      }
    }
  }
}
)";
    const std::string expected = long_code_text();
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("long-page", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "LongPage", 600.0, 400.0);
    std::int64_t now = 1'000'000;
    const auto frames = [&](const int count) {
        for (int frame = 0; frame < count; ++frame)
            static_cast<void>(surface->frame(now += 16'000'000));
    };
    const auto record = [&](const std::string& key) -> const ui::LayoutRecord* {
        const ui::RetainedNode* node = surface->tree().find_key(key);
        return node != nullptr ? surface->layout().find(node->identity()) : nullptr;
    };
    frames(4);
    check(realized_code_rows(*surface, "page.code") <= 3U,
          "a code view sized to its content realized rows the page does not show (" +
              std::to_string(realized_code_rows(*surface, "page.code")) + ")");
    const ui::LayoutRecord* first_row = record("page.code.r0.t");
    check(first_row != nullptr, "the first row of a code view sized to its content was missing");
    const double line_height = first_row->content_bounds.height / 80.0;
    const double full = 4000.0 * line_height + 22.0;
    const ui::LayoutRecord* code = record("page.code");
    // Rows not yet realized are estimated from the measured ones: within a line or two.
    check(code != nullptr && std::abs(code->bounds.height - full) < 2.0 * line_height,
          "a code view sized to its content was not as tall as all its lines (" +
              std::to_string(code != nullptr ? code->bounds.height : 0.0) + " of " +
              std::to_string(full) + ")");
    const double first_height = code->bounds.height;
    const ui::LayoutRecord* page = record("page.scroll");
    const ui::LayoutRecord* intro = record("page.intro");
    const ui::LayoutRecord* after = record("page.after");
    check(page != nullptr && intro != nullptr && after != nullptr &&
              std::abs(page->content_size.height -
                       (intro->bounds.height + first_height + after->bounds.height)) < 1.0,
          "the page's scroll extent did not span the whole code view");

    // Scrolled to the middle, the page shows rows from the middle and has let the first go.
    ui::InputOperationResult scrolled;
    check(surface->input().scroll_to("page.scroll", ui::Point{0.0, 2000.0 * line_height}, scrolled),
          "the page did not scroll");
    frames(4);
    check(surface->tree().find_key("page.code.r1920.t") != nullptr &&
              surface->tree().find_key("page.code.r0.t") == nullptr,
          "scrolling the page did not move the code view's realized rows with it");
    check(realized_code_rows(*surface, "page.code") <= 4U,
          "a code view sized to its content realized more than the page shows after scrolling");
    check(std::abs(record("page.code")->bounds.height - first_height) < 2.0 * line_height,
          "a code view sized to its content changed height as its rows were measured");

    // Select all and copy from a row in the middle: the whole file.
    const ui::LayoutRecord* middle = record("page.code.r1920.t");
    const ui::LayoutRecord* viewport = record("page.scroll");
    const double y = std::clamp(viewport->viewport->y + 100.0, middle->content_bounds.y + 1.0,
                                middle->content_bounds.bottom() - 1.0);
    const ui::Point inside{middle->content_bounds.x + 4.0, y};
    std::vector<ui::SurfaceInputEvent> copy;
    copy.emplace_back(ui::PointerInputEvent{inside, ui::PointerEventType::press, 5, 0});
    copy.emplace_back(ui::PointerInputEvent{inside, ui::PointerEventType::release, 5, 0});
    copy.emplace_back(ui::KeyInputEvent{"a", ui::KeyModifiers{false, true, false, false}});
    copy.emplace_back(ui::KeyInputEvent{"c", ui::KeyModifiers{false, true, false, false}});
    static_cast<void>(surface->input().enqueue(std::move(copy)));
    frames(2);
    check(surface->input().clipboard_text() == std::optional<std::string_view>(expected),
          "select-all in a code view sized to its content did not copy the whole file");
}

void test_rotation_draws_and_hits_the_turned_shape(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Rotated {
  root Panel(key: "rotated.root", layout: { kind: "PANEL", width: 200, height: 200 }) {
    Panel(key: "rotated.card", style: style(rotate: 45, background: #FF8800FF, clip: true, effect: effect("blur", radius: 6)), layout: { width: 100, height: 100, placement: { x: 50, y: 50 } }) {
      Panel(key: "rotated.inner", style: style(background: #FFFFFFFF), layout: { width: 100, height: 100 })
    }
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("rotation", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Rotated", 200.0, 200.0);
    static_cast<void>(surface->frame(1'000'000));
    bool turned = false;
    bool clipped_with_turn = false;
    bool in_transform = false;
    for (const ui::RenderCommand& command : surface->render_commands().commands()) {
        if (const auto* push = std::get_if<ui::TransformPushRenderCommand>(&command)) {
            const double half_root = std::sqrt(0.5);
            turned = std::abs(push->m00 - half_root) < 1e-9 &&
                     std::abs(push->m01 + half_root) < 1e-9 &&
                     std::abs(push->m10 - half_root) < 1e-9;
            in_transform = true;
        }
        if (const auto* clip = std::get_if<ui::ClipPushRenderCommand>(&command);
            clip != nullptr && in_transform && clip->rect == ui::Rect{50.0, 50.0, 100.0, 100.0}) {
            // The clip stays in the card's own space so it turns with what it clips.
            clipped_with_turn = true;
        }
    }
    check(turned, "a 45 degree rotation did not emit a rotating transform");
    // The turned blur is masked to its shape: a clip of its own bounds wraps the effect.
    bool masked_effect = false;
    const std::vector<ui::RenderCommand>& stream = surface->render_commands().commands();
    for (std::size_t index = 1U; index + 1U < stream.size(); ++index) {
        const auto* clip = std::get_if<ui::ClipPushRenderCommand>(&stream[index - 1U]);
        const bool effect = std::holds_alternative<ui::BackdropEffectRenderCommand>(stream[index]) ||
                            std::holds_alternative<ui::BlurRegionRenderCommand>(stream[index]);
        masked_effect = masked_effect ||
                        (clip != nullptr && effect &&
                         clip->rect == ui::Rect{50.0, 50.0, 100.0, 100.0} &&
                         std::holds_alternative<ui::ClipPopRenderCommand>(stream[index + 1U]));
    }
    check(masked_effect, "a rotated effect was not masked to its turned shape");
    check(clipped_with_turn, "a rotated clip did not stay in the rotated content's space");
    // The card's corner (52, 52) lies outside the turned square, though inside its box.
    const ui::RetainedNode* corner = surface->input().inspection_target(ui::Point{52.0, 52.0});
    check(corner != nullptr && corner->description().key == std::optional<std::string>("rotated.root"),
          "a point outside a rotated shape but inside its box hit the shape");
    const ui::RetainedNode* middle = surface->input().inspection_target(ui::Point{100.0, 100.0});
    check(middle != nullptr &&
              middle->description().key == std::optional<std::string>("rotated.inner"),
          "the centre of a rotated shape did not hit it");
    const ui::RetainedNode* tip = surface->input().inspection_target(ui::Point{100.0, 35.0});
    check(tip != nullptr && tip->description().key == std::optional<std::string>("rotated.inner"),
          "a turned corner beyond the layout box did not hit the rotated shape");
}

void test_delayed_and_clipboard_actions(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Timers {
  state saved = false;
  state copied = "";
  root Panel(key: "timers.root", layout: { kind: "COLUMN", width: 300, height: 120 }) {
    Button(key: "timers.save", label: "Save",
      onClick: sequence(action("state.set", name: "saved", value: true),
                        delay(200ms, action("state.set", name: "saved", value: false)),
                        action("clipboard.copy", text: "saved at noon")))
    Text(key: "timers.status", text: saved ? "Saved" : "Idle")
    TextBox(key: "timers.paste", bind: copied)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("delayed-actions", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Timers", 300.0, 120.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const auto status = [&] {
        const ui::RetainedNode* node = surface->tree().find_key("timers.status");
        const auto found = node->description().properties.find("text");
        const runtime::Value* value = found->second.value();
        return value != nullptr && value->string() != nullptr ? *value->string() : std::string{};
    };
    const ui::Point button = center(
        surface->layout().find(surface->tree().find_key("timers.save")->identity())->bounds);
    std::vector<ui::SurfaceInputEvent> events;
    events.emplace_back(ui::PointerInputEvent{button, ui::PointerEventType::press, 4, 0});
    events.emplace_back(ui::PointerInputEvent{button, ui::PointerEventType::release, 4, 0});
    static_cast<void>(surface->input().enqueue(std::move(events)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(status() == "Saved", "the immediate step of a sequence with a delay did not run");
    check(surface->input().requires_frame_advance(),
          "a pending delayed action did not keep frames advancing");
    static_cast<void>(surface->frame(now += 100'000'000));
    check(status() == "Saved", "a delayed action ran before its time");
    static_cast<void>(surface->frame(now += 120'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(status() == "Idle", "a delayed action did not run once its time passed");

    // The copied text reaches the clipboard a paste reads.
    const ui::Point field = center(
        surface->layout().find(surface->tree().find_key("timers.paste")->identity())->bounds);
    std::vector<ui::SurfaceInputEvent> paste;
    paste.emplace_back(ui::PointerInputEvent{field, ui::PointerEventType::press, 4, 0});
    paste.emplace_back(ui::PointerInputEvent{field, ui::PointerEventType::release, 4, 0});
    paste.emplace_back(ui::KeyInputEvent{"v", ui::KeyModifiers{false, true, false, false}});
    static_cast<void>(surface->input().enqueue(std::move(paste)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    const ui::RetainedNode* pasted = surface->tree().find_key("timers.paste");
    const auto pasted_property = pasted->description().properties.find("text");
    const runtime::Value* pasted_text = pasted_property != pasted->description().properties.end()
                                            ? pasted_property->second.value()
                                            : nullptr;
    check(pasted_text != nullptr && pasted_text->string() != nullptr &&
              *pasted_text->string() == "saved at noon",
          "clipboard.copy did not put its text where a paste reads it");
}

void test_scroll_snaps_to_the_nearest_child(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Snapping {
  root Panel(key: "snap.root", layout: { kind: "COLUMN", width: 200, height: 200 }) {
    Scroll(key: "snap.scroll", vertical: true, horizontal: false,
           layout: { width: 200, height: 100, scrollSnap: "START" }) {
      Panel(key: "snap.content", layout: { kind: "COLUMN", width: 200 }) {
        Panel(key: "snap.row.0", layout: { width: 200, height: 40 })
        Panel(key: "snap.row.1", layout: { width: 200, height: 40 })
        Panel(key: "snap.row.2", layout: { width: 200, height: 40 })
        Panel(key: "snap.row.3", layout: { width: 200, height: 40 })
        Panel(key: "snap.row.4", layout: { width: 200, height: 40 })
      }
    }
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("scroll-snap", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Snapping", 200.0, 200.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    ui::InputOperationResult scrolled;
    check(surface->input().scroll_to("snap.scroll", ui::Point{0.0, 52.0}, scrolled),
          "snap fixture did not scroll");
    static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->input().scroll_offset("snap.scroll")->y == 52.0,
          "a scroll container snapped while the user was still scrolling");
    for (int frame = 0; frame < 40; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->input().scroll_offset("snap.scroll")->y == 40.0,
          "a START scroll snap did not settle on the nearest row's top");
    for (int frame = 0; frame < 4; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(surface->active_scroll_animation_count() == 0U,
          "a settled scroll snap kept animating");
}

void test_image_fit_keeps_the_aspect_ratio() {
    const std::string source = R"(
overlay Pictures {
  root Panel(key: "fit.root", layout: { kind: "COLUMN", width: 100, height: 300 }) {
    Image(key: "fit.contain", image: "photo", fit: "CONTAIN", layout: { width: 100, height: 100 })
    Image(key: "fit.cover", image: "photo", fit: "COVER", alignX: "START", layout: { width: 100, height: 100 })
    Image(key: "fit.none", image: "photo", fit: "NONE", alignY: "END", layout: { width: 100, height: 100 })
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("image-fit", bundle);
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application
              .compile_and_activate(compiler::ModuleSource{"Pictures.strata", source}, no_imports,
                                    0U)
              .activated(),
          "image fit fixture did not activate");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 100;
    environment.framebuffer_height = 300;
    environment.logical_width = 100.0;
    environment.logical_height = 300.0;
    // A 200x100 photo: twice as wide as it is tall.
    auto images = std::make_shared<const resource::SvgImageRegistry>(
        std::vector<resource::SvgImageResource>{},
        std::map<std::string, resource::ImageDimensions, std::less<>>{
            {"photo", resource::ImageDimensions{200U, 100U}}});
    ui::Surface surface("image-fit", application, runtime::LayerRole::overlay, "Pictures",
                        environment, {}, ui::WidgetRegistry{}, ui::BehaviorRegistry{}, nullptr,
                        ui::Theme{}, {}, images);
    static_cast<void>(surface.frame(1'000'000));
    std::vector<ui::ImageRenderCommand> drawn;
    for (const ui::RenderCommand& command : surface.render_commands().commands()) {
        if (const auto* image = std::get_if<ui::ImageRenderCommand>(&command))
            drawn.push_back(*image);
    }
    check(drawn.size() == 3U, "image fit fixture did not draw its three images");
    check(drawn[0].bounds == ui::Rect{0.0, 25.0, 100.0, 50.0} &&
              drawn[0].source == ui::TextureRegion{},
          "CONTAIN did not letterbox the whole image at its aspect ratio");
    check(drawn[1].bounds == ui::Rect{0.0, 100.0, 100.0, 100.0} &&
              drawn[1].source == ui::TextureRegion{0.0, 0.0, 0.5, 1.0},
          "COVER aligned to START did not fill by cropping the image's right half");
    check(drawn[2].bounds == ui::Rect{0.0, 200.0, 100.0, 100.0} &&
              drawn[2].source == ui::TextureRegion{0.25, 0.0, 0.5, 1.0},
          "NONE did not draw the image at its natural size, centred and cropped");
}

void test_calendar_picks_days_by_pointer_and_keyboard(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Dates {
  state picked = "2024-03-15";
  root Panel(key: "dates.root", layout: { kind: "COLUMN", width: 320, height: 360 }) {
    Calendar(key: "dates.calendar", bind: picked, today: "2024-03-08", max: "2024-04-10",
             firstDayOfWeek: 1)
    Text(key: "dates.picked", text: picked)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("calendar", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Dates", 320.0, 360.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const ui::RetainedNode* calendar = surface->tree().find_key("dates.calendar");
    check(calendar != nullptr, "calendar fixture is missing its Calendar");
    const auto targets = [&] { return surface->input().subtargets(calendar->identity()); };
    std::vector<ui::WidgetSubtarget> shown = targets();
    check(shown.size() == 44U, "a Calendar did not project its month buttons and 42 days");
    // March 2024 begins on a Friday; a Monday-first grid starts on 26 February.
    check(shown[2].id == "day:2024-02-26" && shown[43].id == "day:2024-04-07",
          "a Monday-first month view did not start on the week's Monday");
    const auto picked = [&] {
        const ui::RetainedNode* node = surface->tree().find_key("dates.picked");
        const auto found = node->description().properties.find("text");
        return found->second.value() != nullptr && found->second.value()->string() != nullptr
                   ? *found->second.value()->string()
                   : std::string{};
    };
    const auto click = [&](const std::string_view id) {
        for (const ui::WidgetSubtarget& target : targets()) {
            if (target.id != id)
                continue;
            std::vector<ui::SurfaceInputEvent> events;
            events.emplace_back(
                ui::PointerInputEvent{center(target.bounds), ui::PointerEventType::press, 4, 0});
            events.emplace_back(
                ui::PointerInputEvent{center(target.bounds), ui::PointerEventType::release, 4, 0});
            static_cast<void>(surface->input().enqueue(std::move(events)));
            static_cast<void>(surface->frame(now += 16'000'000));
            static_cast<void>(surface->frame(now += 16'000'000));
            return true;
        }
        return false;
    };
    check(click("day:2024-03-20") && picked() == "2024-03-20",
          "clicking a day did not pick it through the bound state");
    std::vector<ui::SurfaceInputEvent> keys;
    keys.emplace_back(ui::KeyInputEvent{"right"});
    keys.emplace_back(ui::KeyInputEvent{"down"});
    keys.emplace_back(ui::KeyInputEvent{"pagedown"});
    keys.emplace_back(ui::KeyInputEvent{"enter"});
    static_cast<void>(surface->input().enqueue(std::move(keys)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(picked() == "2024-04-28" || picked() == "2024-03-20",
          "keyboard navigation produced an unexpected day");
    // 28 April is past max, so Enter must not pick it: the selection stays.
    check(picked() == "2024-03-20", "Enter picked a day outside the Calendar's range");
    check(targets()[2].id == "day:2024-04-01", "Page Down did not bring the next month on screen");
    check(click("next") && targets()[2].id == "day:2024-04-29",
          "the next-month button did not advance the month");
    check(!click("day:2024-05-20") || picked() == "2024-03-20",
          "a day after max was picked by a click");
}

void test_calendar_ranges_and_localized_names(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Trips {
  state picked = { start: "", end: "" };
  root Panel(key: "trips.root", layout: { kind: "COLUMN", width: 320, height: 400 }) {
    Calendar(key: "trips.calendar", selection: "RANGE", defaultValue: "2024-03-01",
             monthNames: ["Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran", "Temmuz",
                          "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"],
             weekdayNames: ["Pz", "Pt", "Sa", "Ça", "Pe", "Cu", "Ct"],
             onRangeChange: action("state.setFromEvent", name: "picked"))
    Text(key: "trips.range", text: (picked.start ?? "") + ".." + (picked.end ?? ""))
    Text(key: "trips.date", text: formatDate(1709287205123, "EEEE d MMMM yyyy",
                                             monthNames: ["Ocak", "Şubat", "Mart", "Nisan", "Mayıs",
                                                          "Haziran", "Temmuz", "Ağustos", "Eylül",
                                                          "Ekim", "Kasım", "Aralık"],
                                             weekdayNames: ["Pazar", "Pazartesi", "Salı", "Çarşamba",
                                                            "Perşembe", "Cuma", "Cumartesi"]))
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("calendar-ranges", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Trips", 320.0, 400.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const ui::RetainedNode* calendar = surface->tree().find_key("trips.calendar");
    const auto text_of = [&](const std::string_view key) {
        const ui::RetainedNode* node = surface->tree().find_key(key);
        const auto found = node->description().properties.find("text");
        return found->second.value() != nullptr && found->second.value()->string() != nullptr
                   ? *found->second.value()->string()
                   : std::string{};
    };
    check(text_of("trips.date") == "Cuma 1 Mart 2024",
          "formatDate did not use the month and weekday names it was given");
    const std::vector<ui::WidgetSubtarget> shown = surface->input().subtargets(calendar->identity());
    check(shown.size() == 44U && shown[2].label == "Şubat 26, 2024",
          "a Calendar did not label its days with the month names it was given");
    const auto click = [&](const std::string_view id) {
        for (const ui::WidgetSubtarget& target : surface->input().subtargets(calendar->identity())) {
            if (target.id != id)
                continue;
            std::vector<ui::SurfaceInputEvent> events;
            events.emplace_back(
                ui::PointerInputEvent{center(target.bounds), ui::PointerEventType::press, 4, 0});
            events.emplace_back(
                ui::PointerInputEvent{center(target.bounds), ui::PointerEventType::release, 4, 0});
            static_cast<void>(surface->input().enqueue(std::move(events)));
            static_cast<void>(surface->frame(now += 16'000'000));
            static_cast<void>(surface->frame(now += 16'000'000));
            return;
        }
        check(false, "range fixture is missing a day");
    };
    click("day:2024-03-18");
    check(text_of("trips.range") == "2024-03-18..",
          "the first pick of a range did not start it with an open end");
    click("day:2024-03-11");
    check(text_of("trips.range") == "2024-03-11..2024-03-18",
          "the second pick did not end the range, ordering it earliest first");
    std::size_t band_days = 0U;
    for (const ui::RenderCommand& command : surface->render_commands().commands())
        band_days += std::holds_alternative<ui::SolidRectRenderCommand>(command) ? 1U : 0U;
    check(band_days >= 8U, "a finished range did not band the days it covers");
    click("day:2024-03-25");
    check(text_of("trips.range") == "2024-03-25..",
          "a pick after a finished range did not start a new one");
}

void test_theme_tokens_resolve_inside_nested_values(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Nested {
  root Panel(key: "nested.root", layout: { kind: "COLUMN", width: 200, height: 200 }) {
    Panel(key: "nested.card",
          style: style(background: theme.surfaceRaised, radius: 6,
                       border: { width: 2, color: theme.danger },
                       shadows: [{ color: theme.accent, radius: 4 }]),
          layout: { width: 100, height: 60 })
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("nested-tokens", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Nested", 200.0, 200.0);
    static_cast<void>(surface->frame(1'000'000));
    const ui::ThemeTokens tokens;
    bool border = false;
    bool shadow = false;
    for (const ui::RenderCommand& command : surface->render_commands().commands()) {
        if (const auto* rect = std::get_if<ui::RoundedRectRenderCommand>(&command);
            rect != nullptr && rect->border.has_value() && rect->border->width == 2.0) {
            border = rect->border->paint.representative() == tokens.danger;
        }
        if (const auto* line = std::get_if<ui::BorderRenderCommand>(&command);
            line != nullptr && line->border.width == 2.0) {
            border = line->border.paint.representative() == tokens.danger;
        }
        if (const auto* cast = std::get_if<ui::ShadowRenderCommand>(&command)) {
            shadow = cast->color.red == tokens.accent.red && cast->color.green == tokens.accent.green &&
                     cast->color.blue == tokens.accent.blue;
        }
    }
    check(border, "a theme token inside a border did not resolve to the theme's colour");
    check(shadow, "a theme token inside a shadow list did not resolve to the theme's colour");
}

void test_timers_fire_on_their_interval(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Ticks {
  state count = 0;
  state once = 0;
  state paused = 0;
  root Panel(
    key: "ticks.root",
    layout: { kind: "COLUMN", width: 200, height: 100 },
    timers: [
      { id: "count", every: 100ms, action: action("state.set", name: "count", value: count + 1) },
      { id: "once", every: 250ms, repeat: false, action: action("state.set", name: "once", value: once + 1) },
      { id: "paused", every: 50ms, running: false, action: action("state.set", name: "paused", value: paused + 1) }
    ]
  ) {
    Text(key: "ticks.count", text: format("{0}", count))
    Text(key: "ticks.once", text: format("{0}", once))
    Text(key: "ticks.paused", text: format("{0}", paused))
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("timers", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Ticks", 200.0, 100.0);
    const auto shown = [&](const std::string_view key) {
        const ui::RetainedNode* node = surface->tree().find_key(key);
        const auto found = node->description().properties.find("text");
        return found->second.value() != nullptr && found->second.value()->string() != nullptr
                   ? *found->second.value()->string()
                   : std::string{};
    };
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    // A second at twenty milliseconds a frame: ten intervals of the repeating timer.
    for (int frame = 0; frame < 50; ++frame)
        static_cast<void>(surface->frame(now += 20'000'000));
    check(shown("ticks.count") == "10",
          "a repeating timer did not fire once each interval: " + shown("ticks.count"));
    check(shown("ticks.once") == "1", "a one-shot timer did not fire exactly once");
    check(shown("ticks.paused") == "0", "a timer that is not running fired");
    // A stalled frame (five intervals at once) fires once, not five times.
    static_cast<void>(surface->frame(now += 500'000'000));
    static_cast<void>(surface->frame(now += 1'000'000));
    check(shown("ticks.count") == "11", "a stalled frame fired a burst of timer actions");
}

void test_chart_inspects_selects_and_hides_series(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Sales {
  state picked = { label: "", index: -1 };
  root Panel(key: "sales.root", layout: { kind: "COLUMN", width: 480, height: 600 }) {
    Chart(key: "sales.line", labels: ["Jan", "Feb", "Mar", "Apr"],
          series: [{ name: "Revenue", values: [10, 20, 15, 30] },
                   { name: "Costs", values: [5, null, 8, 12] }],
          onSelect: action("state.setFromEvent", name: "picked"))
    Chart(key: "sales.pie", kind: "PIE", labels: ["Big", "Small"], values: [75, 25],
          onSelect: action("state.setFromEvent", name: "picked"))
    Text(key: "sales.picked", text: picked.label ?? "")
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("charts", bundle);
    // Motion on: hiding a series fades it out.
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Sales", 480.0, 600.0, false);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const ui::RetainedNode* line = surface->tree().find_key("sales.line");
    const ui::RetainedNode* pie = surface->tree().find_key("sales.pie");
    check(line != nullptr && pie != nullptr, "chart fixture is missing a Chart");
    const auto picked = [&] {
        const ui::RetainedNode* node = surface->tree().find_key("sales.picked");
        const auto found = node->description().properties.find("text");
        return found->second.value() != nullptr && found->second.value()->string() != nullptr
                   ? *found->second.value()->string()
                   : std::string{};
    };
    const auto press = [&](const ui::Point at) {
        std::vector<ui::SurfaceInputEvent> events;
        events.emplace_back(ui::PointerInputEvent{at, ui::PointerEventType::press, 4, 0});
        events.emplace_back(ui::PointerInputEvent{at, ui::PointerEventType::release, 4, 0});
        static_cast<void>(surface->input().enqueue(std::move(events)));
        static_cast<void>(surface->frame(now += 16'000'000));
        static_cast<void>(surface->frame(now += 16'000'000));
    };
    const auto target = [&](const ui::RetainedNode& owner, const std::string_view id) {
        for (const ui::WidgetSubtarget& candidate : surface->input().subtargets(owner.identity())) {
            if (candidate.id == id)
                return candidate;
        }
        check(false, "chart fixture is missing a subtarget");
        return ui::WidgetSubtarget{};
    };

    const std::vector<ui::WidgetSubtarget> shown = surface->input().subtargets(line->identity());
    check(shown.size() == 6U && shown[2].id == "point:2" && shown[2].label == "Mar" &&
              shown[5].id == "legend:1",
          "a line Chart did not project its four categories and two legend entries");
    press(center(target(*line, "point:2").bounds));
    check(picked() == "Mar", "clicking a category did not select it through onSelect");

    std::vector<ui::SurfaceInputEvent> keys;
    keys.emplace_back(ui::KeyInputEvent{"right"});
    keys.emplace_back(ui::KeyInputEvent{"enter"});
    static_cast<void>(surface->input().enqueue(std::move(keys)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(picked() == "Apr", "Right then Enter did not select the next category");

    // Each series' line is one opaque colour; hiding one fades that colour out before it goes.
    const auto strokes = [&](const bool opaque) {
        std::vector<std::uint32_t> colors;
        for (const ui::RenderCommand& command : surface->render_commands().commands()) {
            const auto* path = std::get_if<ui::PathRenderCommand>(&command);
            if (path == nullptr || !path->shape.stroke.has_value())
                continue;
            const runtime::ColorValue* color = path->shape.stroke->color();
            if (color == nullptr || color->alpha == 0U || (color->alpha == 255U) != opaque)
                continue;
            colors.push_back((std::uint32_t{color->red} << 16U) |
                             (std::uint32_t{color->green} << 8U) | color->blue);
        }
        return colors;
    };
    const std::vector<std::uint32_t> lines = strokes(true);
    const auto fading = [&] {
        return std::ranges::any_of(strokes(false), [&](const std::uint32_t color) {
            return std::ranges::find(lines, color) != lines.end();
        });
    };
    check(!fading(), "a line Chart drew a series line part way faded before any change");
    press(center(target(*line, "legend:1").bounds));
    check(target(*line, "legend:1").label == "Show Costs",
          "clicking a legend entry did not hide its series");
    check(fading(), "hiding a series did not fade its line out");
    for (int frame = 0; frame < 60; ++frame)
        static_cast<void>(surface->frame(now += 16'000'000));
    check(!fading() && strokes(true).size() < lines.size(),
          "a hidden series' line did not finish fading out");
    press(center(target(*line, "legend:1").bounds));
    check(target(*line, "legend:1").label == "Hide Costs",
          "clicking a hidden series' legend entry did not show it again");

    // The small slice is the top-left quarter: its box's corner lies outside the circle.
    const ui::WidgetSubtarget small = target(*pie, "slice:1");
    check(small.shape.size() > 3U, "a pie slice did not project its outline");
    press(ui::Point{small.bounds.x + 2.0, small.bounds.y + 2.0});
    check(picked() == "Apr", "a click outside a slice's outline selected it");
    press(ui::Point{small.bounds.x + small.bounds.width * 0.6,
                    small.bounds.y + small.bounds.height * 0.6});
    check(picked() == "Small", "a click inside a slice did not select it");
}

void test_markdown_expands_blocks_and_follows_links(const std::filesystem::path& resources) {
    const std::vector<ui::MarkdownBlock> blocks = ui::parse_markdown(
        "# Title\n\nSome **bold** and *em* `code` ~~gone~~ [site](https://a.dev \"t\").\n\n"
        "- one\n  - nested\n- [x] done\n\n3. three\n4. four\n\n> quoted\n\n```\nx = 1\n```\n\n"
        "| a | b |\n|---|---|\n| 1 | 2 |\n\n---\n");
    check(blocks.size() == 8U && blocks[0].kind == ui::MarkdownBlockKind::heading &&
              blocks[0].level == 1 && blocks[2].kind == ui::MarkdownBlockKind::list &&
              blocks[3].ordered && blocks[3].start == 3 &&
              blocks[4].kind == ui::MarkdownBlockKind::quote &&
              blocks[5].code == "x = 1" && blocks[6].rows.size() == 2U &&
              blocks[7].kind == ui::MarkdownBlockKind::rule,
          "Markdown did not parse its block structure");
    const std::vector<ui::MarkdownSpan>& spans = blocks[1].spans;
    const auto styled = [&spans](const std::string_view text) {
        const auto found = std::ranges::find(spans, text, &ui::MarkdownSpan::text);
        return found != spans.end() ? &*found : nullptr;
    };
    check(styled("bold") != nullptr && styled("bold")->strong && styled("em")->emphasis &&
              styled("code")->code && styled("gone")->strike &&
              styled("site")->href == "https://a.dev",
          "Markdown did not parse its inline emphasis, code, strikethrough and links");
    check(blocks[2].items.size() == 2U && blocks[2].items[0].blocks.size() == 2U &&
              blocks[2].items[1].checked == std::optional<bool>(true),
          "Markdown did not nest a list or read a task item");

    const std::string source = R"(
overlay Notes {
  state followed = "";
  root Panel(key: "notes.root", layout: { kind: "COLUMN", width: 480, height: 300 }) {
    Markdown(key: "notes.text", text: "Read [the docs](https://strata.dev/docs) first.",
             onLink: action("state.setFromEvent", name: "followed"))
    Text(key: "notes.followed", text: followed)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("markdown", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Notes", 480.0, 300.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const ui::RetainedNode* paragraph = surface->tree().find_key("notes.text.0");
    check(paragraph != nullptr && paragraph->description().type == "RichText",
          "Markdown did not expand its paragraph into RichText");
    const std::vector<ui::WidgetSubtarget> links = surface->input().subtargets(paragraph->identity());
    check(links.size() == 1U && links[0].kind == ui::WidgetSubtargetKind::link,
          "a Markdown link did not project one link target");
    std::vector<ui::SurfaceInputEvent> events;
    events.emplace_back(ui::PointerInputEvent{center(links[0].bounds), ui::PointerEventType::press, 4, 0});
    events.emplace_back(
        ui::PointerInputEvent{center(links[0].bounds), ui::PointerEventType::release, 4, 0});
    static_cast<void>(surface->input().enqueue(std::move(events)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    const ui::RetainedNode* followed = surface->tree().find_key("notes.followed");
    const auto found = followed->description().properties.find("text");
    check(found->second.value() != nullptr && found->second.value()->string() != nullptr &&
              *found->second.value()->string() == "https://strata.dev/docs",
          "following a Markdown link did not carry its destination to onLink");
}

void test_modal_sides_open_drawers_and_sheets(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Drawers {
  root Panel(key: "drawers.root", layout: { kind: "OVERLAY", width: 800, height: 500 }) {
    Modal(key: "drawers.left", side: "RIGHT") {
      Panel(key: "drawers.panel", layout: { kind: "COLUMN", width: 240 })
    }
    Modal(key: "drawers.sheet", side: "BOTTOM") {
      Panel(key: "drawers.sheetPanel", layout: { kind: "COLUMN", height: 120 })
    }
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("drawers", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Drawers", 800.0, 500.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    static_cast<void>(surface->frame(now += 1'000'000'000));
    const auto bounds = [&](const std::string_view key) {
        const ui::LayoutRecord* record =
            surface->layout().find(surface->tree().find_key(key)->identity());
        check(record != nullptr, "drawer fixture node has no layout");
        return record->bounds;
    };
    const ui::Rect drawer = bounds("drawers.panel");
    check(drawer.x == 560.0 && drawer.y == 0.0 && drawer.width == 240.0 && drawer.height == 500.0,
          "a RIGHT Modal did not hold its content full height against the right edge");
    const ui::Rect sheet = bounds("drawers.sheetPanel");
    check(sheet.x == 0.0 && sheet.y == 380.0 && sheet.width == 800.0 && sheet.height == 120.0,
          "a BOTTOM Modal did not hold its content full width against the bottom edge");
    const auto transition = surface->tree().find_key("drawers.panel")->description().properties.find(
        "transition");
    check(transition != surface->tree().find_key("drawers.panel")->description().properties.end() &&
              transition->second.value() != nullptr &&
              transition->second.value()->string() != nullptr &&
              *transition->second.value()->string() == "strata.drawer.right",
          "a drawer's content did not slide from its edge by default");
}

void test_date_picker_opens_picks_and_closes(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Due {
  state due = "2024-03-15";
  root Panel(key: "due.root", layout: { kind: "COLUMN", width: 480, height: 480 }) {
    DatePicker(key: "due.picker", bind: due, max: "2024-03-28", format: "d MMMM yyyy")
    Text(key: "due.picked", text: due)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("date-picker", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Due", 480.0, 480.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const ui::RetainedNode* picker = surface->tree().find_key("due.picker");
    check(picker != nullptr, "date picker fixture is missing its DatePicker");
    const auto targets = [&] { return surface->input().subtargets(picker->identity()); };
    const auto press = [&](const ui::Point at) {
        std::vector<ui::SurfaceInputEvent> events;
        events.emplace_back(ui::PointerInputEvent{at, ui::PointerEventType::press, 4, 0});
        events.emplace_back(ui::PointerInputEvent{at, ui::PointerEventType::release, 4, 0});
        static_cast<void>(surface->input().enqueue(std::move(events)));
        static_cast<void>(surface->frame(now += 16'000'000));
        static_cast<void>(surface->frame(now += 16'000'000));
    };
    const auto find = [&](const std::string_view id) -> std::optional<ui::WidgetSubtarget> {
        for (const ui::WidgetSubtarget& target : targets()) {
            if (target.id == id)
                return target;
        }
        return std::nullopt;
    };
    const auto picked = [&] {
        const auto found = surface->tree().find_key("due.picked")->description().properties.find("text");
        return *found->second.value()->string();
    };
    check(targets().size() == 1U, "a closed DatePicker projected more than its field");
    press(center(find("$control")->bounds));
    check(find("day:2024-03-20").has_value() && find("day:2024-03-20")->detached,
          "clicking a DatePicker did not open its month dropdown");
    check(!find("day:2024-03-30")->enabled, "a day after max was offered in the dropdown");
    press(center(find("day:2024-03-20")->bounds));
    check(picked() == "2024-03-20", "picking a day did not update the bound date");
    check(targets().size() == 1U, "picking a day did not close the dropdown");
    std::vector<ui::SurfaceInputEvent> keys;
    keys.emplace_back(ui::KeyInputEvent{"enter"});
    keys.emplace_back(ui::KeyInputEvent{"right"});
    keys.emplace_back(ui::KeyInputEvent{"enter"});
    static_cast<void>(surface->input().enqueue(std::move(keys)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(picked() == "2024-03-21", "Enter, Right, Enter did not pick the next day");
    const auto text = ui::date_picker_text(*picker, ui::calendar_state(*picker));
    check(text == std::optional<std::string>("21 March 2024"),
          "a DatePicker did not show its day in its format");
}

void test_range_slider_drags_the_nearer_thumb(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Prices {
  state range = { start: 20, end: 80 };
  root Panel(key: "prices.root", layout: { kind: "COLUMN", width: 300, height: 120 }) {
    RangeSlider(key: "prices.slider", min: 0, max: 100, step: 5, minGap: 10,
                defaultStart: 20, defaultEnd: 80, layout: { width: 220, height: 28 },
                onChange: action("state.setFromEvent", name: "range"))
    Text(key: "prices.shown", text: format("{0}-{1}", range.start, range.end))
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("range-slider", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Prices", 300.0, 120.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const ui::RetainedNode* slider = surface->tree().find_key("prices.slider");
    check(slider != nullptr, "range slider fixture is missing its RangeSlider");
    const ui::Rect track = ui::range_slider_track(surface->layout().find(slider->identity())->bounds);
    const auto at = [&](const double value) {
        return ui::Point{track.x + track.width * value / 100.0, track.y + track.height * 0.5};
    };
    const auto state = [&] { return ui::range_slider_state(*slider); };
    std::vector<ui::SurfaceInputEvent> drag;
    drag.emplace_back(ui::PointerInputEvent{at(70.0), ui::PointerEventType::press, 4, 0});
    drag.emplace_back(ui::PointerInputEvent{at(50.0), ui::PointerEventType::move, 4, 0});
    drag.emplace_back(ui::PointerInputEvent{at(10.0), ui::PointerEventType::move, 4, 0});
    drag.emplace_back(ui::PointerInputEvent{at(10.0), ui::PointerEventType::release, 4, 0});
    static_cast<void>(surface->input().enqueue(std::move(drag)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(state().start == 20.0 && state().end == 30.0,
          "dragging the end thumb did not stop the minimum gap above the start");
    check(surface->input().focused(slider->identity()), "pressing a RangeSlider did not focus it");
    std::vector<ui::SurfaceInputEvent> keys;
    keys.emplace_back(ui::KeyInputEvent{"right"});
    keys.emplace_back(ui::KeyInputEvent{"tab", ui::KeyModifiers{true, false, false, false}});
    keys.emplace_back(ui::KeyInputEvent{"left"});
    static_cast<void>(surface->input().enqueue(std::move(keys)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(state().start == 15.0 && state().end == 35.0,
          "the keys did not move the end thumb, then the start thumb after Shift+Tab (" +
              std::to_string(state().start) + ".." + std::to_string(state().end) + ")");
    const auto shown = surface->tree().find_key("prices.shown")->description().properties.find("text");
    check(*shown->second.value()->string() == "15-35",
          "the RangeSlider's onChange did not carry { start, end }");
}

void test_color_picker_picks_by_field_strip_swatch_and_keys(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay Colors {
  state accent = #FF0000;
  root Panel(key: "colors.root", layout: { kind: "COLUMN", width: 300, height: 320 }) {
    ColorPicker(key: "colors.picker", bind: accent, swatches: [#00FF00, #0000FF])
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("color-picker", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "Colors", 300.0, 320.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    const ui::RetainedNode* picker = surface->tree().find_key("colors.picker");
    check(picker != nullptr, "color picker fixture is missing its ColorPicker");
    const auto state = [&] { return ui::color_picker_state(*picker); };
    const ui::ColorPickerProjection projection = ui::color_picker_projection(
        state(), surface->layout().find(picker->identity())->bounds);
    const auto press = [&](const ui::Point at) {
        std::vector<ui::SurfaceInputEvent> events;
        events.emplace_back(ui::PointerInputEvent{at, ui::PointerEventType::press, 4, 0});
        events.emplace_back(ui::PointerInputEvent{at, ui::PointerEventType::release, 4, 0});
        static_cast<void>(surface->input().enqueue(std::move(events)));
        static_cast<void>(surface->frame(now += 16'000'000));
        static_cast<void>(surface->frame(now += 16'000'000));
    };
    check(state().hex() == "#FF0000" && state().hsv.saturation == 1.0,
          "a ColorPicker did not start from its bound color");
    // Halfway along the field's top edge: half-saturated, full value, still red.
    press(ui::Point{projection.field.x + projection.field.width * 0.5, projection.field.y});
    check(state().hex() == "#FF8080", "a field press did not set saturation and value");
    press(ui::Point{projection.hue.x + projection.hue.width / 3.0,
                    projection.hue.y + projection.hue.height * 0.5});
    check(state().hex() == "#80FF80", "a hue strip press did not turn the hue to green");
    press(center(projection.swatches[1]));
    check(state().hex() == "#0000FF", "a swatch press did not pick its color");
    std::vector<ui::SurfaceInputEvent> keys;
    keys.emplace_back(ui::KeyInputEvent{"down", ui::KeyModifiers{true, false, false, false}});
    keys.emplace_back(ui::KeyInputEvent{"left", ui::KeyModifiers{true, false, false, false}});
    static_cast<void>(surface->input().enqueue(std::move(keys)));
    static_cast<void>(surface->frame(now += 16'000'000));
    static_cast<void>(surface->frame(now += 16'000'000));
    check(state().hex() == "#1717E6", "Shift+Down and Shift+Left did not step value and saturation");
}

void test_palette_projection_is_cached_across_frames(const std::filesystem::path& resources) {
    std::string commands;
    for (int index = 0; index < 40; ++index) {
        commands += "    Command(id: \"palette.command." + std::to_string(index) +
                    "\", label: \"Command number " + std::to_string(index) +
                    "\", action: action(\"notification.raise\", message: \"run\"))\n";
    }
    const std::string source = R"(
overlay PaletteFrames {
  root Panel(key: "palette.root", layout: { kind: "COLUMN", width: 400, height: 300 }) {
)" + commands + R"(
    CommandPalette(key: "palette.frames", maxVisibleRows: 8)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("palette-frames", bundle);
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application
              .compile_and_activate(compiler::ModuleSource{"palette-frames.strata", source},
                                    no_imports, 0U)
              .activated(),
          "palette frames fixture did not activate");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 400;
    environment.framebuffer_height = 300;
    environment.logical_width = 400.0;
    environment.logical_height = 300.0;
    environment.reduced_motion = true;
    environment.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, false,
    };
    check(environment.input.ime, "palette frames fixture needs IME placement");
    ui::Surface surface(
        "palette-frames", application, runtime::LayerRole::overlay, "PaletteFrames", environment,
        ui::TextEngine::load_control_font(
            resources, resource::ResourceId::parse("assets/strata/fonts/medium.ttf")));
    std::int64_t now = 1'000'000;
    static_cast<void>(surface.frame(now));
    static_cast<void>(surface.dispatch_action(
        "palette.set",
        object({{"key", runtime::Value(runtime::KeyValue{"palette.frames"})},
                {"open", runtime::Value(true)}}),
        "test", std::nullopt, runtime::Value{}));
    static_cast<void>(surface.frame(now += 16'000'000));
    static_cast<void>(surface.input().text("number"));
    static_cast<void>(surface.frame(now += 16'000'000));
    check(surface.input().focused_key() == std::optional<std::string_view>("palette.frames"),
          "palette frames fixture did not focus its open palette");

    // Frames that change nothing the projection depends on reuse it: placing the IME caret and
    // scrolling the query editor must not re-rank every command each frame.
    std::size_t projections = 0U;
    for (int frame = 0; frame < 6; ++frame) {
        // Requests a frame without changing anything (focus stays uncontained).
        static_cast<void>(surface.set_focus_containment(std::nullopt));
        const ui::SurfaceFrame frame_result = surface.frame(now += 16'000'000);
        projections += frame_result.operations.subtarget_projections;
    }
    check(projections == 0U, "an unchanged palette was re-projected every frame");
}

void test_environment_set_rebuilds_once(const std::filesystem::path& resources) {
    const std::string source = R"(
overlay EnvironmentOnce {
  root Panel(key: "environment.root", layout: { kind: "COLUMN", width: 600, height: 200 }) {
    Button(key: "environment.inset", label: "Inset", onClick: action("environment.set", safeLeft: 200))
    Text(key: "environment.class", text: env.viewport)
  }
}
)";
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("environment-once", bundle);
    std::unique_ptr<ui::Surface> surface =
        compiled_surface(application, resources, source, "EnvironmentOnce", 640.0, 240.0);
    std::int64_t now = 1'000'000;
    static_cast<void>(surface->frame(now));
    check(surface->viewport_class() == "regular", "environment fixture is not a regular viewport");

    // A click sets insets that make the viewport compact. The frame routing it rebuilds once,
    // with the new class, and the next frame has nothing left to rebuild.
    static_cast<void>(surface->input().enqueue_click("environment.inset"));
    const ui::SurfaceFrame changed = surface->frame(now += 16'000'000);
    check(changed.operations.rebuilds == 1U && surface->viewport_class() == "compact",
          "environment.set did not rebuild into the compact class");
    const ui::RetainedNode* label = surface->tree().find_key("environment.class");
    const auto text = label != nullptr ? label->description().properties.find("text")
                                       : ui::DescriptionNode::Properties::const_iterator{};
    check(label != nullptr && text != label->description().properties.end() &&
              text->second.value() != nullptr && text->second.value()->string() != nullptr &&
              *text->second.value()->string() == "compact",
          "the rebuilt description did not read the new viewport class");
    const ui::SurfaceFrame after = surface->frame(now += 16'000'000);
    check(after.operations.rebuilds == 0U,
          "environment.set rebuilt the surface again a frame later for its viewport class");
}

} // namespace

int strata_test_interaction_residual(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2)
            throw std::invalid_argument("expected resource root");
        const std::filesystem::path resources(arguments[1]);
        const std::shared_ptr<const runtime::ApplicationBundle> bundle = load_bundle();
        InputFixture fixture(bundle, resources);
        test_primary_pointer_focus_default(fixture);
        test_detached_portal_hit_testing(fixture);
        test_authored_popup_dismissal(fixture);
        test_lifecycle_input_hooks(fixture);
        test_rich_text_press_arm(fixture);
        test_static_text_state_partition(fixture);
        test_wrapped_static_text_navigation(fixture);
        test_wrapped_editor_pointer_navigation(fixture);
        test_editor_text_scroll(fixture);
        test_static_text_container_owner_transition(fixture);
        test_nested_editor_does_not_activate_section(fixture);
        test_slider_pointer_matches_rendered_track(fixture);
        test_choice_semantics(fixture);
        test_tooltip_disclosure(fixture);
        test_manipulation_slop(fixture);
        test_movable_snapping(fixture);
        test_movable_fling_snap_and_bounds(fixture);
        test_layout_observers(fixture);
        test_backdrop_observer(fixture);
        test_scale_handle(fixture);
        test_passive_descendant_activation(fixture);
        test_release_target_activation(fixture);
        test_select_inside_modal(fixture);
        test_banner_semantics(fixture);
        test_event_outcome_pairing(fixture);
        test_removed_focused_editor_commits(fixture);
        test_zoom_view_keys(fixture);
        test_virtual_focused_row_stays_realized(resources);
        test_post_layout_reveal_reaches_layout(resources);
        test_sticky_table_header_blocks_hits(resources);
        test_subtargets_hit_in_presented_space(resources);
        test_item_grid_header_clip(resources);
        test_frame_input_prepares_are_explained(resources);
        test_queued_batch_sees_rebuilt_tree(resources);
        test_hover_follows_content_under_still_pointer(resources);
        test_pointer_cursor_follows_what_is_hovered(resources);
        test_text_decoration_and_named_weight(resources);
        test_per_corner_radius_rounds_each_corner(resources);
        test_text_list_and_math_helpers(resources);
        test_number_bytes_duration_and_relative_time_formats(resources);
        test_paint_rotate_spins_a_gradient_border(resources);
        test_code_view_highlights_numbers_and_copies(resources);
        test_code_view_selection_spans_unrealized_rows(resources);
        test_content_sized_code_view_realizes_what_the_page_shows(resources);
        test_rotation_draws_and_hits_the_turned_shape(resources);
        test_delayed_and_clipboard_actions(resources);
        test_scroll_snaps_to_the_nearest_child(resources);
        test_image_fit_keeps_the_aspect_ratio();
        test_calendar_picks_days_by_pointer_and_keyboard(resources);
        test_chart_inspects_selects_and_hides_series(resources);
        test_timers_fire_on_their_interval(resources);
        test_markdown_expands_blocks_and_follows_links(resources);
        test_modal_sides_open_drawers_and_sheets(resources);
        test_date_picker_opens_picks_and_closes(resources);
        test_range_slider_drags_the_nearer_thumb(resources);
        test_color_picker_picks_by_field_strip_swatch_and_keys(resources);
        test_calendar_ranges_and_localized_names(resources);
        test_theme_tokens_resolve_inside_nested_values(resources);
        test_palette_projection_is_cached_across_frames(resources);
        test_environment_set_rebuilds_once(resources);
        std::cout << "strata_interaction_residual_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_interaction_residual_tests: " << error.what() << '\n';
        return 1;
    }
}
