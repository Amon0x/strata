#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
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
#include "ui/input/detail.hpp"

namespace strata::ui {
namespace {

using input_detail::string_value;

constexpr std::string_view file_drop_behavior_id = "strata.fileDrop";
constexpr std::string_view window_drag_behavior_id = "strata.windowDrag";
constexpr std::string_view channel_behavior_id = "strata.channel";
constexpr std::string_view drag_out_behavior_id = "strata.dragOut";
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
           id.starts_with("node.") || id.starts_with("audio.") || id.starts_with("http.") ||
           id.starts_with("socket.") || id.starts_with("file.") || id == "clipboard.copyImage" ||
           id == "clipboard.readImage";
}

[[nodiscard]] data::JsonValue rectangle_json(const Rect& rectangle) {
    return data::JsonValue(data::JsonValue::Object{
        {"height", data::JsonValue(static_cast<std::int64_t>(rectangle.height))},
        {"width", data::JsonValue(static_cast<std::int64_t>(rectangle.width))},
        {"x", data::JsonValue(static_cast<std::int64_t>(rectangle.x))},
        {"y", data::JsonValue(static_cast<std::int64_t>(rectangle.y))},
    });
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
        // `fromEvent` names the field the event fills: a slider's value as a window's opacity.
        if (const std::string* field = string_value(action.payload.field("fromEvent"));
            field != nullptr && !field->empty() && *field != "kind" && *field != "request" &&
            event.value.kind() != runtime::ValueKind::null_value) {
            std::erase_if(payload, [field](const auto& entry) {
                return entry.first == *field || entry.first == "fromEvent";
            });
            payload.emplace_back(*field, runtime::value_to_json(event.value));
        }
    }
    // The host reads back pixels, not nodes: it gets the node's rectangle on its framebuffer.
    if (id == "node.capture") {
        if (const std::string* key = string_value(action.payload.field("key")); key != nullptr) {
            if (const std::optional<Rect> rectangle = capture_rectangle(*key);
                rectangle.has_value()) {
                payload.emplace_back("rect", rectangle_json(*rectangle));
            }
        }
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

void InputRouter::arm_drag_out(const PointerInputEvent& event, const RetainedNode* target) {
    drag_out_press_.reset();
    for (const RetainedNode* node = target; node != nullptr; node = node->parent()) {
        if (attached(*node, drag_out_behavior_id) != nullptr) {
            drag_out_press_ = DragOutPress{event.pointer_id, node->identity(), event.position};
            return;
        }
    }
}

bool InputRouter::start_drag_out(const PointerInputEvent& event) {
    if (!drag_out_press_.has_value() || drag_out_press_->pointer_id != event.pointer_id)
        return false;
    if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
        drag_out_press_.reset();
        return false;
    }
    if (event.type != PointerEventType::move)
        return false;
    const double delta_x = event.position.x - drag_out_press_->position.x;
    const double delta_y = event.position.y - drag_out_press_->position.y;
    const double slop = input_config_.pointer_drag_slop;
    if (!event.coalesced_moved_beyond_slop && delta_x * delta_x + delta_y * delta_y <= slop * slop)
        return false;
    const std::uint64_t identity = drag_out_press_->node_identity;
    drag_out_press_.reset();
    const RetainedNode* const node = tree_ != nullptr ? tree_->find_identity(identity) : nullptr;
    const DescriptionBehavior* const behavior =
        node != nullptr ? attached(*node, drag_out_behavior_id) : nullptr;
    if (behavior == nullptr)
        return false;
    // What goes out: files as paths, text as text. A node with neither has nothing to carry.
    data::JsonValue::Array files;
    if (const runtime::Value* listed = behavior->options.field("files");
        listed != nullptr && listed->list() != nullptr) {
        for (const runtime::Value& file : listed->list()->values) {
            if (file.string() != nullptr && !file.string()->empty())
                files.emplace_back(*file.string());
        }
    }
    const std::string* const text = string_value(behavior->options.field("text"));
    if (files.empty() && (text == nullptr || text->empty()))
        return false;
    const std::uint64_t request = ++next_platform_request_;
    data::JsonValue::Object payload{
        {"files", data::JsonValue(std::move(files))},
        {"kind", data::JsonValue("system.dragOut")},
        {"request", data::JsonValue(static_cast<std::int64_t>(request))},
    };
    if (text != nullptr && !text->empty())
        payload.emplace_back("text", data::JsonValue(*text));
    if (const runtime::Value* move = behavior->options.field("move");
        move != nullptr && move->boolean() != nullptr) {
        payload.emplace_back("move", data::JsonValue(*move->boolean()));
    }
    if (host_services_->emit_effect(platform_effect_id, data::encode_canonical_json(data::JsonValue(
                                                            std::move(payload)))) != 0U) {
        return false;
    }
    pending_drag_outs_.insert_or_assign(request, identity);
    return true;
}

std::optional<Rect> InputRouter::capture_rectangle(const std::string_view key) const {
    if (tree_ == nullptr || layout_ == nullptr)
        return std::nullopt;
    const RetainedNode* const node = tree_->find_key(key);
    const LayoutRecord* const record = node != nullptr ? layout_->find(node->identity()) : nullptr;
    if (record == nullptr)
        return std::nullopt;
    prepare_pointer_geometry();
    // Where it is drawn: turned, scaled and moved as presented, and cut by the scroll views and
    // clips above it.
    Rect bounds = presented_bounds(*node, record->bounds);
    if (const auto entry = pointer_hit_index_.find(node->identity());
        entry != pointer_hit_index_.end()) {
        if (const std::optional<Rect>& clip = pointer_hit_entries_[entry->second].traversal_clip;
            clip.has_value()) {
            bounds = bounds.clip_intersection(*clip);
        }
    }
    const double scale = layout_->scale > 0.0 ? layout_->scale : 1.0;
    const double left = std::max(0.0, std::floor(bounds.x * scale));
    const double top = std::max(0.0, std::floor(bounds.y * scale));
    const double right = std::ceil((bounds.x + bounds.width) * scale);
    const double bottom = std::ceil((bounds.y + bounds.height) * scale);
    if (right <= left || bottom <= top)
        return std::nullopt;
    return Rect{left, top, right - left, bottom - top};
}

InputOperationResult InputRouter::platform(PlatformInputEvent event) {
    InputOperationResult result;
    if (tree_ == nullptr)
        return result;
    using Fields = std::vector<std::pair<std::string, runtime::Value>>;
    if (event.kind == PlatformInputKind::event) {
        // Every node listening on the channel hears what was said on it, in tree order.
        std::string channel;
        runtime::Value data;
        try {
            const data::JsonValue said = data::parse_json(event.result_json);
            const data::JsonValue* name = said.find("channel");
            if (name == nullptr || name->string() == nullptr)
                return result;
            channel = *name->string();
            if (const data::JsonValue* value = said.find("data"); value != nullptr)
                data = runtime::value_from_json(*value);
        } catch (const std::exception&) {
            return result;
        }
        const std::vector<RetainedNode*> listeners = tree_->behavior_nodes();
        for (RetainedNode* const node : listeners) {
            if (tree_->find_identity(node->identity()) != node)
                continue;
            for (const DescriptionBehavior& behavior : node->description().behaviors) {
                if (!behavior.enabled || behavior.id != channel_behavior_id)
                    continue;
                const runtime::Value* listens = behavior.options.field("channel");
                if (listens == nullptr || listens->string() == nullptr ||
                    *listens->string() != channel)
                    continue;
                // `match` narrows it to what has these fields: one tray menu item, one message
                // type.
                if (const runtime::Value* match = behavior.options.field("match");
                    match != nullptr && match->object() != nullptr) {
                    const bool matches =
                        std::ranges::all_of(match->object()->fields, [&data](const auto& field) {
                            const runtime::Value* said = data.field(field.first);
                            return said != nullptr && *said == field.second;
                        });
                    if (!matches)
                        continue;
                }
                BehaviorInputScope scope(*this, *node, behavior, InputEventPhase::target, result);
                static_cast<void>(scope.emit("channel", data));
            }
        }
        return result;
    }
    if (event.kind == PlatformInputKind::result) {
        // A drag out ended: its node's action hears how it was dropped.
        if (const auto drag = pending_drag_outs_.find(event.request);
            drag != pending_drag_outs_.end()) {
            const std::uint64_t identity = drag->second;
            pending_drag_outs_.erase(drag);
            RetainedNode* const node = tree_->find_identity(identity);
            const DescriptionBehavior* const behavior =
                node != nullptr ? attached(*node, drag_out_behavior_id) : nullptr;
            if (behavior == nullptr)
                return result;
            runtime::Value answer;
            try {
                answer = runtime::value_from_json(data::parse_json(event.result_json));
            } catch (const std::exception&) {
                answer = runtime::Value{};
            }
            BehaviorInputScope scope(*this, *node, *behavior, InputEventPhase::target, result);
            static_cast<void>(scope.emit("drag-out", std::move(answer)));
            return result;
        }
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
