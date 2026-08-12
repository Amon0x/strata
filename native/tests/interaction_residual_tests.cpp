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

#include "data/json.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "runtime/expression.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/command.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/notification.hpp"
#include "ui/semantics.hpp"
#include "ui/status.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/choice_model.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/shell_model.hpp"
#include "ui/widget/subtarget.hpp"

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
    runtime::Value materialized = object({{"text", runtime::Value(text)}});
    std::vector<std::pair<std::string, runtime::ExpressionValue>> fields;
    fields.emplace_back("text", runtime::ExpressionValue(runtime::Value(std::move(text))));
    fields.emplace_back("action", runtime::ExpressionValue(std::move(action)));
    return runtime::ExpressionValue(
        std::make_shared<const runtime::ExpressionObjectValue>(runtime::ExpressionObjectValue{
            std::move(materialized),
            std::move(fields),
        }));
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
    const runtime::Value materialized(std::vector<runtime::Value>{
        object({{"text", runtime::Value("first")}}),
        object({{"text", runtime::Value(" second")}}),
    });
    ui::DescriptionNode::Properties rich = sized(300.0, 32.0);
    rich.emplace("text", runtime::ExpressionValue(runtime::Value("first second")));
    rich.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
    rich.emplace("spans",
                 runtime::ExpressionValue(std::make_shared<const runtime::ExpressionListValue>(
                     runtime::ExpressionListValue{
                         materialized,
                         std::move(expressed),
                     })));
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
    const runtime::Value materialized(std::vector<runtime::Value>{
        object({{"text", runtime::Value("link only")}}),
    });

    ui::DescriptionNode::Properties ordinary = sized(240.0, 28.0);
    ordinary.emplace("text", runtime::ExpressionValue(runtime::Value("ordinary title")));
    ui::DescriptionNode::Properties opted_out = sized(240.0, 28.0);
    opted_out.emplace("text", runtime::ExpressionValue(runtime::Value("decorative title")));
    opted_out.emplace("selectable", runtime::ExpressionValue(runtime::Value(false)));
    opted_out.emplace("selectionContainer", runtime::ExpressionValue(runtime::Value("document")));
    ui::DescriptionNode::Properties linked = sized(240.0, 28.0);
    linked.emplace("text", runtime::ExpressionValue(runtime::Value("link only")));
    linked.emplace("selectable", runtime::ExpressionValue(runtime::Value(false)));
    linked.emplace("spans",
                   runtime::ExpressionValue(std::make_shared<const runtime::ExpressionListValue>(
                       runtime::ExpressionListValue{
                           materialized,
                           std::move(expressed),
                       })));
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
    check(ordinary_node != nullptr && fixture.input_.focused_key().has_value() &&
              *fixture.input_.focused_key() == "static.ordinary" &&
              fixture.input_.static_text_selection_snapshot(ordinary_node->identity()).has_value(),
          "absent selectable stopped preserving the frozen selectable=true default");
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

void test_static_text_container_owner_transition(InputFixture& fixture) {
    ui::DescriptionNode::Properties first = sized(160.0, 30.0);
    first.emplace("text", runtime::ExpressionValue(runtime::Value("alpha")));
    first.emplace("selectionContainer", runtime::ExpressionValue(runtime::Value("document")));
    ui::DescriptionNode::Properties second = sized(160.0, 30.0);
    second.emplace("text", runtime::ExpressionValue(runtime::Value("omega")));
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
    static_assert(ui::tooltip_default_show_delay_nanos == 400'000'000);
    static_assert(ui::tooltip_default_hide_delay_nanos == 80'000'000);
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
    check(ui::tooltip_disclosure_visible(*owner) && !fixture.input_.requires_frame_advance(),
          "Tooltip did not preserve disclosure while focus remained inside its anchor");
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
        test_rich_text_press_arm(fixture);
        test_static_text_state_partition(fixture);
        test_wrapped_static_text_navigation(fixture);
        test_wrapped_editor_pointer_navigation(fixture);
        test_static_text_container_owner_transition(fixture);
        test_nested_editor_does_not_activate_section(fixture);
        test_slider_pointer_matches_rendered_track(fixture);
        test_choice_semantics(fixture);
        test_tooltip_disclosure(fixture);
        test_manipulation_slop(fixture);
        test_passive_descendant_activation(fixture);
        test_banner_semantics(fixture);
        std::cout << "strata_interaction_residual_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_interaction_residual_tests: " << error.what() << '\n';
        return 1;
    }
}
