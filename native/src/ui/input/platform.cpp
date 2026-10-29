#include "ui/input.hpp"

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/json.hpp"
#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/behavior/input.hpp"

namespace strata::ui {
namespace {

constexpr std::string_view file_drop_behavior_id = "strata.fileDrop";
constexpr std::string_view window_drag_behavior_id = "strata.windowDrag";
/** The effect platform requests reach the host as. */
constexpr std::string_view platform_effect_id = "strata.platform";

[[nodiscard]] const DescriptionBehavior* attached(const RetainedNode& node,
                                                  const std::string_view id) noexcept {
    for (const DescriptionBehavior& behavior : node.description().behaviors) {
        if (behavior.enabled && behavior.id == id)
            return &behavior;
    }
    return nullptr;
}

[[nodiscard]] bool platform_action(const std::string_view id) noexcept {
    return id.starts_with("system.") || id.starts_with("window.") || id.starts_with("media.") ||
           id == "clipboard.copyImage" || id == "clipboard.readImage";
}

} // namespace

std::optional<runtime::ActionDispatchOutcome>
InputRouter::execute_platform_action(const runtime::ActionEvent& event,
                                     const runtime::Action& action) {
    const std::string& id = action.id();
    if (!platform_action(id))
        return std::nullopt;
    const std::uint64_t request = ++next_platform_request_;
    data::JsonValue::Object payload{
        {"kind", data::JsonValue(id)},
        {"request", data::JsonValue(static_cast<std::int64_t>(request))},
    };
    if (const runtime::ValueObject* fields = action.payload.object()) {
        for (const auto& [name, value] : fields->fields) {
            if (name != "kind" && name != "request" && name != "event")
                payload.emplace_back(name, runtime::value_to_json(value));
        }
        // What ran it: a `then` answer, a drop's files, a slider's value. Requests may fall back on
        // it.
        if (event.value.kind() != runtime::ValueKind::null_value)
            payload.emplace_back("event", runtime::value_to_json(event.value));
    }
    const std::uint32_t status = host_services_->emit_effect(
        platform_effect_id, data::encode_canonical_json(data::JsonValue(std::move(payload))));
    if (status != 0U) {
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::failed,
            id,
            {"strata.surface.platform"},
            "The host does not provide platform services (the strata.platform effect).",
        };
    }
    last_platform_request_ = request;
    return runtime::ActionDispatchOutcome{
        runtime::ActionDispatchStatus::handled,
        id,
        {"strata.surface.platform"},
        std::nullopt,
    };
}

void InputRouter::start_window_drag(const PointerInputEvent& event, const RetainedNode* target) {
    // The nearest drag region above the pressed node, unless a control comes first: a button in a
    // title bar is pressed, not dragged by.
    for (const RetainedNode* node = target; node != nullptr; node = node->parent()) {
        if (attached(*node, window_drag_behavior_id) != nullptr) {
            const bool double_press = last_window_drag_press_nanos_ != 0 &&
                                      event.timestamp_nanos - last_window_drag_press_nanos_ <=
                                          input_config_.multi_click_interval_nanos;
            last_window_drag_press_nanos_ = double_press ? 0 : event.timestamp_nanos;
            const std::string kind = double_press ? "window.toggleMaximize" : "window.drag";
            const std::uint64_t request = ++next_platform_request_;
            static_cast<void>(host_services_->emit_effect(
                platform_effect_id,
                data::encode_canonical_json(data::JsonValue(data::JsonValue::Object{
                    {"kind", data::JsonValue(kind)},
                    {"request", data::JsonValue(static_cast<std::int64_t>(request))},
                }))));
            return;
        }
        if (focusable(*node))
            return;
    }
}

InputOperationResult InputRouter::platform(PlatformInputEvent event) {
    InputOperationResult result;
    if (tree_ == nullptr)
        return result;
    using Fields = std::vector<std::pair<std::string, runtime::Value>>;
    if (event.kind == PlatformInputKind::result) {
        const auto pending = pending_continuations_.find(event.request);
        if (pending == pending_continuations_.end())
            return result;
        const PendingContinuation continuation = std::move(pending->second);
        pending_continuations_.erase(pending);
        // The node that asked may have gone, and its answer with it.
        RetainedNode* const node = tree_->find_identity(continuation.node_identity);
        if (node == nullptr)
            return result;
        runtime::Value answer;
        try {
            answer = runtime::value_from_json(data::parse_json(event.result_json));
        } catch (const std::exception&) {
            answer = runtime::Value{};
        }
        const runtime::ActionEvent answered{"platform-result", node->description().key,
                                            std::move(answer)};
        static_cast<void>(execute_action(answered, *continuation.action, *node, result));
        return result;
    }

    // Files over the surface: the nearest node that takes them.
    RetainedNode* target = nullptr;
    if (event.kind != PlatformInputKind::file_leave && layout_ != nullptr) {
        prepare_pointer_geometry();
        for (RetainedNode* node = hit_test(event.position); node != nullptr;
             node = node->parent()) {
            if (attached(*node, file_drop_behavior_id) != nullptr) {
                target = node;
                break;
            }
        }
    }
    std::vector<runtime::Value> paths;
    paths.reserve(event.files.size());
    for (std::string& path : event.files)
        paths.emplace_back(std::move(path));
    const auto emit = [this, &result](RetainedNode& node, const std::string_view kind,
                                      runtime::Value value) {
        const DescriptionBehavior* const behavior = attached(node, file_drop_behavior_id);
        if (behavior == nullptr)
            return;
        BehaviorInputScope scope(*this, node, *behavior, InputEventPhase::target, result);
        static_cast<void>(scope.emit(kind, std::move(value)));
    };
    const std::optional<std::uint64_t> previous = file_drop_target_;
    const std::optional<std::uint64_t> next =
        target != nullptr && event.kind == PlatformInputKind::file_drag
            ? std::optional<std::uint64_t>(target->identity())
            : std::nullopt;
    file_drop_target_ = next;
    // The node the files have left (or been dropped elsewhere than) hears they are gone.
    if (previous.has_value() && previous != next &&
        (event.kind != PlatformInputKind::file_drop || target == nullptr ||
         target->identity() != *previous)) {
        if (RetainedNode* const left = tree_->find_identity(*previous); left != nullptr) {
            emit(*left, "files-left",
                 runtime::Value(Fields{{"over", runtime::Value(false)},
                                       {"dropped", runtime::Value(false)},
                                       {"files", runtime::Value(std::vector<runtime::Value>{})}}));
        }
    }
    if (target == nullptr)
        return result;
    if (event.kind == PlatformInputKind::file_drag) {
        // Once as they arrive: moving within the node changes nothing it shows.
        if (previous != next) {
            emit(*target, "files-over",
                 runtime::Value(Fields{{"over", runtime::Value(true)},
                                       {"dropped", runtime::Value(false)},
                                       {"files", runtime::Value(std::move(paths))}}));
        }
    } else if (event.kind == PlatformInputKind::file_drop) {
        emit(*target, "files-dropped",
             runtime::Value(Fields{{"over", runtime::Value(false)},
                                   {"dropped", runtime::Value(true)},
                                   {"files", runtime::Value(std::move(paths))}}));
    }
    return result;
}

} // namespace strata::ui
