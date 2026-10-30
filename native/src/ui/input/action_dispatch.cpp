#include "ui/input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/behavior/backdrop.hpp"
#include "ui/behavior/input.hpp"
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;

void InputOperationResult::record(JsonValue event, JsonValue outcome) {
    events.push_back(std::move(event));
    action_outcomes.push_back(std::move(outcome));
}

std::size_t InputOperationResult::reserve(JsonValue event) {
    const std::size_t index = events.size();
    record(std::move(event), JsonValue{});
    return index;
}

void InputRouter::track_pointer(const PointerInputEvent& event, InputOperationResult& result) {
    using Fields = std::vector<std::pair<std::string, runtime::Value>>;
    const auto observer = [](const RetainedNode& node) -> const DescriptionBehavior* {
        for (const DescriptionBehavior& behavior : node.description().behaviors) {
            if (behavior.enabled && behavior.id == pointer_behavior_id)
                return &behavior;
        }
        return nullptr;
    };
    if (tree_ == nullptr)
        return;
    const bool present = event.type != PointerEventType::cancel;
    const std::int64_t now = event.timestamp_nanos > 0 ? event.timestamp_nanos : frame_time_nanos_;
    // A press near the last one, soon after it, with the same button, counts another click.
    if (event.type == PointerEventType::press) {
        const bool again = last_pointer_click_.count > 0 &&
                           last_pointer_click_.button == event.button &&
                           now - last_pointer_click_.timestamp_nanos < 500'000'000 &&
                           std::abs(event.position.x - last_pointer_click_.position.x) < 6.0 &&
                           std::abs(event.position.y - last_pointer_click_.position.y) < 6.0;
        last_pointer_click_ = PointerClick{event.position, now, event.button,
                                           again ? last_pointer_click_.count + 1 : 1};
    }
    std::set<std::uint64_t>& captured = pointer_captures_[event.pointer_id];
    if (event.type == PointerEventType::press) {
        captured.clear();
        for (const std::uint64_t identity : hovered_) {
            const RetainedNode* node = tree_->find_identity(identity);
            if (node != nullptr && observer(*node) != nullptr)
                captured.insert(identity);
        }
    }
    const auto listening = [&](const std::uint64_t identity) {
        return hovered_.contains(identity) || captured.contains(identity);
    };
    // Nodes the pointer has left, or every one when it has left the surface.
    for (auto tracked = pointer_trackers_.begin(); tracked != pointer_trackers_.end();) {
        if (present && listening(*tracked)) {
            ++tracked;
            continue;
        }
        const std::uint64_t identity = *tracked;
        tracked = pointer_trackers_.erase(tracked);
        RetainedNode* const node = tree_->find_identity(identity);
        const DescriptionBehavior* const behavior = node != nullptr ? observer(*node) : nullptr;
        if (behavior == nullptr)
            continue;
        BehaviorInputScope scope(*this, *node, *behavior, InputEventPhase::target, result);
        static_cast<void>(
            scope.emit("pointer-left", runtime::Value(Fields{{"inside", runtime::Value(false)},
                                                             {"phase", runtime::Value("leave")},
                                                             {"pressed", runtime::Value(false)}})));
    }
    if (!present || layout_ == nullptr) {
        pointer_captures_.erase(event.pointer_id);
        return;
    }
    prepare_pointer_geometry();
    // Where the pointer is over each hovered observer, and each one a held press began over, in
    // its own logical coordinates and as a fraction of its box: the node's hit bounds as it
    // shows, transforms included. A press and its release say so, with the button and keys.
    std::set<std::uint64_t> route(hovered_.begin(), hovered_.end());
    route.insert(captured.begin(), captured.end());
    const std::string_view kind = event.type == PointerEventType::press     ? "pointer-pressed"
                                  : event.type == PointerEventType::release ? "pointer-released"
                                                                            : "pointer-moved";
    const std::string_view phase = event.type == PointerEventType::press     ? "press"
                                   : event.type == PointerEventType::release ? "release"
                                                                             : "move";
    const bool held = event.type == PointerEventType::press ||
                      (event.type == PointerEventType::move && !captured.empty());
    for (const std::uint64_t identity : route) {
        RetainedNode* const node = tree_->find_identity(identity);
        const DescriptionBehavior* const behavior = node != nullptr ? observer(*node) : nullptr;
        const auto entry = pointer_hit_index_.find(identity);
        if (behavior == nullptr || entry == pointer_hit_index_.end())
            continue;
        const PointerHitEntry& hit = pointer_hit_entries_[entry->second];
        if (!(hit.bounds.width > 0.0) || !(hit.bounds.height > 0.0))
            continue;
        const double u = (event.position.x - hit.bounds.x) / hit.bounds.width;
        const double v = (event.position.y - hit.bounds.y) / hit.bounds.height;
        pointer_trackers_.insert(identity);
        BehaviorInputScope scope(*this, *node, *behavior, InputEventPhase::target, result);
        static_cast<void>(scope.emit(
            kind, runtime::Value(Fields{
                      {"inside", runtime::Value(u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0)},
                      {"x", runtime::Value(u * hit.local_bounds.width)},
                      {"y", runtime::Value(v * hit.local_bounds.height)},
                      {"u", runtime::Value(u)},
                      {"v", runtime::Value(v)},
                      {"width", runtime::Value(hit.local_bounds.width)},
                      {"height", runtime::Value(hit.local_bounds.height)},
                      {"surfaceX", runtime::Value(event.position.x)},
                      {"surfaceY", runtime::Value(event.position.y)},
                      {"phase", runtime::Value(std::string(phase))},
                      {"pressed", runtime::Value(held)},
                      {"button", runtime::Value(static_cast<double>(event.button))},
                      {"clickCount", runtime::Value(static_cast<double>(
                                         event.type == PointerEventType::move
                                             ? 0
                                             : last_pointer_click_.count))},
                      {"shift", runtime::Value(event.modifiers.shift)},
                      {"control", runtime::Value(event.modifiers.control)},
                      {"alt", runtime::Value(event.modifiers.alt)},
                      {"meta", runtime::Value(event.modifiers.super_key)},
                  })));
    }
    if (event.type == PointerEventType::release)
        pointer_captures_.erase(event.pointer_id);
}

void InputRouter::observe_backdrop(const std::uint64_t probe, const double luminance,
                                   InputOperationResult& result) {
    RetainedNode* const node = tree_ != nullptr ? tree_->find_identity(probe) : nullptr;
    const DescriptionBehavior* const observer =
        node != nullptr ? backdrop_observer(*node) : nullptr;
    if (observer == nullptr || !std::isfinite(luminance))
        return;
    const auto threshold = [observer](const std::string_view name, const double fallback) {
        const runtime::Value* const value = observer->options.field(name);
        return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
                   ? std::clamp(*value->number(), 0.0, 1.0)
                   : fallback;
    };
    const double light = threshold("light", default_backdrop_light_threshold);
    const double dark = std::min(threshold("dark", default_backdrop_dark_threshold), light);
    const runtime::Value* const dwell_option = observer->options.field("dwell");
    const int dwell = dwell_option != nullptr && dwell_option->number() != nullptr
                          ? std::clamp(static_cast<int>(std::lround(*dwell_option->number())), 1, 60)
                          : default_backdrop_dwell;
    const runtime::Value* const retained = node->retained_value(backdrop_light_value);
    const bool known = retained != nullptr && retained->boolean() != nullptr;
    const bool was_light = known && *retained->boolean();
    // Between the thresholds the reading holds, so a backdrop hovering near either one cannot
    // flicker. The first report has nothing to hold and takes the nearer side.
    const bool is_light = luminance >= light   ? true
                          : luminance <= dark  ? false
                          : known              ? was_light
                                               : luminance >= (light + dark) * 0.5;
    if (known && is_light == was_light) {
        tree_->set_input_value(probe, backdrop_pending_value, runtime::Value());
        return;
    }
    // A changed reading must hold for `dwell` consecutive reports before it is believed. Host
    // readback runs a frame or two behind, and a frame in which a scene or a surface is changing
    // can measure something neither the old nor the new scene will: without the dwell, such a
    // frame flips the scheme and the next one flips it straight back.
    if (known && dwell > 1) {
        const runtime::Value* const pending = node->retained_value(backdrop_pending_value);
        const int streak =
            pending != nullptr && pending->number() != nullptr ? static_cast<int>(*pending->number()) : 0;
        // The streak is signed by the reading it counts: positive light, negative dark.
        const int agreed = is_light ? std::max(streak, 0) + 1 : std::min(streak, 0) - 1;
        if (std::abs(agreed) < dwell) {
            tree_->set_input_value(probe, backdrop_pending_value,
                                   runtime::Value(static_cast<double>(agreed)));
            return;
        }
    }
    tree_->set_input_value(probe, backdrop_pending_value, runtime::Value());
    tree_->set_input_value(probe, backdrop_light_value, runtime::Value(is_light));
    tree_->set_input_value(probe, backdrop_luminance_value, runtime::Value(luminance));
    // An unreported node already presents as dark, so a first dark reading is not a change.
    if (!known && !is_light)
        return;
    BehaviorInputScope scope(*this, *node, *observer, InputEventPhase::target, result);
    static_cast<void>(scope.emit("backdrop-changed", runtime::Value(is_light)));
}

runtime::ActionDispatchOutcome InputRouter::emit(
    JsonValue event,
    const std::shared_ptr<const runtime::ActionValue>& action,
    const RetainedNode& node,
    runtime::Value event_value,
    InputOperationResult& result
) {
    JsonValue outcome_event = event;
    // The outcome is known only after the action ran, and the action can record pairs of its own
    // (focus moves, nested dispatch): the slot is taken now so this pair keeps the event's place.
    const std::size_t slot = result.reserve(std::move(event));
    if (action == nullptr || action->action == nullptr) {
        result.action_outcomes[slot] = no_action_outcome(outcome_event);
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::no_action, std::nullopt, {}, std::nullopt,
        };
    }
    const std::string event_kind =
        outcome_event.find("type") != nullptr && outcome_event.find("type")->string() != nullptr
            ? *outcome_event.find("type")->string()
            : std::string("event");
    std::optional<std::string> lifecycle_owner;
    if (event_kind == "tree-children-requested" && event_value.key() != nullptr &&
        !event_value.key()->value.empty()) {
        lifecycle_owner = "item:" + event_value.key()->value;
    }
    const runtime::ActionEvent action_event{
        event_kind,
        node.description().key,
        std::move(event_value),
        std::move(lifecycle_owner),
    };
    const runtime::ActionDispatchOutcome outcome = execute_action(
        action_event,
        *action,
        node,
        result
    );
    if (outcome.message.has_value() && !outcome.message->empty()) {
        status_feedback_.publish(*outcome.message);
    }
    report_action_outcome(*action->action, outcome, node);
    std::vector<JsonValue> owners;
    for (const std::string& owner : outcome.handler_owners) owners.emplace_back(owner);
    result.action_outcomes[slot] = object({
        {"actionId", outcome.action_id.has_value() ? JsonValue(*outcome.action_id) : JsonValue{}},
        {"event", std::move(outcome_event)},
        {"handlerOwners", array(std::move(owners))},
        {"message", outcome.message.has_value() ? JsonValue(*outcome.message) : JsonValue{}},
        {"status", JsonValue(std::string(status_name(outcome.status)))},
    });
    return outcome;
}

runtime::ActionDispatchOutcome InputRouter::emit(
    JsonValue event,
    const std::shared_ptr<const runtime::Action>& action,
    const RetainedNode& node,
    runtime::Value event_value,
    InputOperationResult& result
) {
    return emit(
        std::move(event),
        action != nullptr
            ? std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
                  action,
                  std::nullopt,
                  {},
              })
            : nullptr,
        node,
        std::move(event_value),
        result
    );
}

InjectedActionResult InputRouter::dispatch_action(
    std::string action_id,
    runtime::Value payload,
    std::string event_kind,
    std::optional<std::string> source_key,
    runtime::Value event_value,
    const bool dynamic
) {
    if (tree_ == nullptr || tree_->root() == nullptr) {
        throw std::invalid_argument("action injection requires a prepared retained tree");
    }
    if (action_id.empty() || event_kind.empty()) {
        throw std::invalid_argument("action injection requires an action id and event kind");
    }
    RetainedNode* source_node = source_key.has_value()
        ? tree_->find_key(*source_key)
        : tree_->root();
    if (source_node == nullptr) {
        throw std::invalid_argument("action injection source key is not retained by the surface");
    }
    std::shared_ptr<const runtime::Action> action;
    if (dynamic) {
        action = std::make_shared<const runtime::Action>(
            runtime::dynamic_action(std::move(action_id), std::move(payload))
        );
    } else {
        const std::shared_ptr<const runtime::ActionContract> contract =
            application_.bundle()->action_registry().contract(action_id);
        if (contract == nullptr) {
            throw std::invalid_argument("injected typed action is not declared by the application");
        }
        action = std::make_shared<const runtime::Action>(contract, std::move(payload));
    }
    InputOperationResult input;
    JsonValue event = object({
        {"action", canonical_action(*action, *source_node)},
        {"source", source(*source_node)},
        {"type", JsonValue(event_kind)},
        {"value", runtime::value_to_json(event_value)},
    });
    runtime::ActionDispatchOutcome outcome = emit(
        std::move(event), action, *source_node, std::move(event_value), input
    );
    input.injected_events = 1U;
    input.processed_events = 1U;
    return InjectedActionResult{std::move(input), std::move(outcome)};
}

struct InputRouter::ScriptHost final : runtime::ScriptEffects {
    ScriptHost(InputRouter& router, runtime::ActionEvent event, const std::uint64_t node,
               InputOperationResult& result)
        : router(router), event(std::move(event)), node_identity(node), result(result) {}

    [[nodiscard]] const runtime::Value* state(const runtime::LexicalStateBinding& binding) override {
        return router.application_.state().find(binding.address);
    }

    [[nodiscard]] std::optional<std::string> commit(runtime::ScriptTransaction transaction) override {
        std::vector<runtime::ScriptStateWrite> writes;
        writes.reserve(transaction.writes.size());
        for (runtime::ScriptWrite& write : transaction.writes)
            writes.push_back(runtime::ScriptStateWrite{*write.binding, std::move(write.value)});
        if (std::optional<std::string> rejected = router.application_.commit_script(
                writes, router.public_surface_id_, transaction.undo_label,
                transaction.undo_coalesce, router.frame_time_nanos_)) {
            return rejected;
        }
        // The handler's actions run from its node; one that left the tree takes them with it.
        for (const std::shared_ptr<const runtime::ActionValue>& action : transaction.actions) {
            const RetainedNode* source = node();
            if (source == nullptr || action == nullptr)
                break;
            const runtime::ActionDispatchOutcome outcome =
                router.execute_action(event, *action, *source, result);
            if (action->action != nullptr)
                router.report_action_outcome(*action->action, outcome, *source);
        }
        return std::nullopt;
    }

    [[nodiscard]] runtime::ScriptAnswer
    perform(const std::shared_ptr<const runtime::ActionValue>& action) override {
        const RetainedNode* source = node();
        if (source == nullptr || action == nullptr) {
            return runtime::ScriptAnswer{runtime::ScriptAnswer::Kind::failed, {},
                                         "The handler's node left the interface."};
        }
        router.last_platform_request_.reset();
        router.last_answer_.reset();
        const runtime::ActionDispatchOutcome outcome =
            router.execute_action(event, *action, *source, result);
        if (outcome.status == runtime::ActionDispatchStatus::failed ||
            outcome.status == runtime::ActionDispatchStatus::unhandled) {
            router.last_answer_.reset();
            router.last_platform_request_.reset();
            return runtime::ScriptAnswer{
                runtime::ScriptAnswer::Kind::failed, {},
                outcome.message.value_or("Action " + outcome.action_id.value_or("?") + " " +
                                         std::string(status_name(outcome.status)) + ".")};
        }
        // An answer known at once (the clock) is the await's value, as a host's answer is.
        if (router.last_answer_.has_value()) {
            runtime::Value answer = std::move(*router.last_answer_);
            router.last_answer_.reset();
            return runtime::ScriptAnswer{runtime::ScriptAnswer::Kind::value, std::move(answer), {}};
        }
        if (router.last_platform_request_.has_value()) {
            request = *router.last_platform_request_;
            router.last_platform_request_.reset();
            return runtime::ScriptAnswer{runtime::ScriptAnswer::Kind::pending, {}, {}};
        }
        return runtime::ScriptAnswer{};
    }

    [[nodiscard]] const RetainedNode* node() const {
        return router.tree_ != nullptr ? router.tree_->find_identity(node_identity) : nullptr;
    }

    InputRouter& router;
    runtime::ActionEvent event;
    std::uint64_t node_identity;
    InputOperationResult& result;
    /** The platform request the run waits on. */
    std::optional<std::uint64_t> request;
};

runtime::ActionDispatchOutcome InputRouter::execute_script(const runtime::ActionEvent& event,
                                                           const runtime::ActionValue& handler,
                                                           const RetainedNode& node,
                                                           InputOperationResult& result) {
    ScriptHost host(*this, event, node.identity(), result);
    runtime::ExpressionRuntime scripts(application_.host(),
                                       application_.bundle()->action_registry());
    runtime::ScriptOutcome outcome = scripts.run_script(handler.script, {event.value}, host);
    return script_outcome(scripts, std::move(outcome), host);
}

void InputRouter::resume_script(const std::shared_ptr<runtime::ScriptRun>& run,
                                const std::uint64_t node_identity,
                                const runtime::ActionEvent& event, runtime::Value answer,
                                InputOperationResult& result) {
    ScriptHost host(*this, event, node_identity, result);
    runtime::ExpressionRuntime scripts(application_.host(),
                                       application_.bundle()->action_registry());
    runtime::ScriptOutcome outcome = scripts.resume_script(run, std::move(answer), false, host);
    static_cast<void>(script_outcome(scripts, std::move(outcome), host));
}

runtime::ActionDispatchOutcome InputRouter::script_outcome(runtime::ExpressionRuntime& scripts,
                                                           runtime::ScriptOutcome outcome,
                                                           const ScriptHost& host) {
    for (const runtime::RuntimeDiagnostic& diagnostic : scripts.diagnostics())
        pending_diagnostics_.push_back(diagnostic);
    const std::string id = runtime::script_action_contract()->id;
    switch (outcome.kind) {
    case runtime::ScriptOutcome::Kind::finished:
        break;
    case runtime::ScriptOutcome::Kind::waiting:
        if (host.request.has_value()) {
            pending_continuations_.insert_or_assign(
                *host.request,
                PendingContinuation{host.node_identity, nullptr, std::move(outcome.run), host.event});
        }
        break;
    case runtime::ScriptOutcome::Kind::failed: {
        const runtime::Value* code = outcome.error.field("code");
        const runtime::Value* message = outcome.error.field("message");
        const std::string text =
            message != nullptr ? runtime::display_string(*message)
                               : "Uncaught " + runtime::display_string(outcome.error) + ".";
        const RetainedNode* source = host.node();
        pending_diagnostics_.push_back(runtime::RuntimeDiagnostic{
            code != nullptr && code->string() != nullptr ? *code->string()
                                                         : std::string("STRATA.SCRIPT.UNCAUGHT"),
            text,
            source != nullptr ? std::string(source->structural_path()) : std::string{},
            std::nullopt,
            runtime::DiagnosticSeverity::error,
            std::nullopt,
        });
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::failed, id, {"strata.script"}, text};
    }
    }
    return runtime::ActionDispatchOutcome{
        runtime::ActionDispatchStatus::handled, id, {"strata.script"}, std::nullopt};
}

runtime::ActionDispatchOutcome InputRouter::execute_action(
    const runtime::ActionEvent& event,
    const runtime::ActionValue& action,
    const RetainedNode& node,
    InputOperationResult& result
) {
    if (action.script != nullptr)
        return execute_script(event, action, node, result);
    if (action.action == nullptr) {
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::failed,
            std::nullopt,
            {"strata.surface.declarative"},
            "Composed action contains an empty step.",
        };
    }
    return action.composition.has_value()
               ? execute_composition(event, action, node, result)
               : execute_action(
                     event,
                     *action.action,
                     node,
                     result,
                     action.lexical_state_binding.has_value()
                         ? &*action.lexical_state_binding
                         : nullptr
                 );
}

runtime::ActionDispatchOutcome InputRouter::execute_action(
    const runtime::ActionEvent& event,
    const runtime::Action& action,
    const RetainedNode& node,
    InputOperationResult& result,
    const runtime::LexicalStateBinding* const lexical_state_binding
) {
    if (action.contract->dispatch_policy == runtime::ActionDispatchPolicy::framework) {
        if (action.id() == "form.validate" || action.id() == "form.submit") {
            return execute_form_action(action, result);
        }
        if (std::optional<runtime::ActionDispatchOutcome> outcome =
                execute_surface_action(event, action, node, result);
            outcome.has_value()) {
            return std::move(*outcome);
        }
    }
    return application_.dispatch(
        event,
        action,
        node.description().state_scope,
        lexical_state_binding,
        public_surface_id_,
        frame_time_nanos_
    );
}

void InputRouter::run_due_delays(InputOperationResult& result) {
    if (pending_delays_.empty())
        return;
    // Steps may schedule further delays, so take the due ones out before running any.
    std::vector<PendingDelay> due;
    std::erase_if(pending_delays_, [this, &due](PendingDelay& pending) {
        if (pending.due_nanos > frame_time_nanos_)
            return false;
        due.push_back(std::move(pending));
        return true;
    });
    std::ranges::stable_sort(due, {}, &PendingDelay::due_nanos);
    for (const PendingDelay& pending : due) {
        // A step whose node left the tree goes with it, as its activation would have.
        const RetainedNode* node =
            tree_ != nullptr ? tree_->find_identity(pending.node_identity) : nullptr;
        if (node == nullptr)
            continue;
        static_cast<void>(execute_action(pending.event, *pending.action, *node, result));
    }
}

void InputRouter::run_timers(InputOperationResult& result) {
    timers_waiting_ = false;
    if (tree_ == nullptr || tree_->root() == nullptr) {
        timers_.clear();
        timer_nodes_.clear();
        return;
    }
    // The nodes that declare timers change only with the tree's generation.
    if (timer_tree_ != tree_ || timer_generation_ != tree_->generation() ||
        timer_structure_generation_ != tree_->structure_generation()) {
        timer_tree_ = tree_;
        timer_generation_ = tree_->generation();
        timer_structure_generation_ = tree_->structure_generation();
        timer_nodes_.clear();
        const auto visit = [this](const auto& self, const RetainedNode& node) -> void {
            if (node.description().properties.contains("timers"))
                timer_nodes_.push_back(node.identity());
            for (const auto& child : node.children())
                self(self, *child);
        };
        visit(visit, *tree_->root());
    }
    std::set<std::pair<std::uint64_t, std::string>> declared;
    for (const std::uint64_t identity : timer_nodes_) {
        const RetainedNode* node = tree_->find_identity(identity);
        if (node == nullptr || node->lifecycle() == RetainedLifecycle::exiting)
            continue;
        const auto found = node->description().properties.find("timers");
        if (found == node->description().properties.end() || found->second.list() == nullptr ||
            *found->second.list() == nullptr) {
            continue;
        }
        for (const runtime::ExpressionValue& element : (*found->second.list())->values) {
            if (element.object() == nullptr || *element.object() == nullptr)
                continue;
            const runtime::ExpressionObjectValue& timer = **element.object();
            const auto data = [&timer](const std::string_view name) -> const runtime::Value* {
                const runtime::ExpressionValue* field = timer.field(name);
                return field != nullptr ? field->value() : nullptr;
            };
            const runtime::Value* id = data("id");
            const runtime::Value* every = data("every");
            const runtime::ExpressionValue* action = timer.field("action");
            if (id == nullptr || id->string() == nullptr || every == nullptr ||
                every->duration() == nullptr || every->duration()->nanoseconds <= 0 ||
                action == nullptr || action->action() == nullptr || *action->action() == nullptr) {
                continue;
            }
            const auto flag = [&data](const std::string_view name, const bool fallback) {
                const runtime::Value* value = data(name);
                return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
            };
            const std::int64_t interval = every->duration()->nanoseconds;
            const bool running = flag("running", true);
            const bool repeat = flag("repeat", true);
            auto key = std::pair{identity, *id->string()};
            declared.insert(key);
            const auto [entry, inserted] = timers_.try_emplace(
                std::move(key), TimerSchedule{interval, frame_time_nanos_ + interval, false});
            TimerSchedule& schedule = entry->second;
            // A paused timer starts its interval over when it runs again, as does a new interval.
            if (!running || schedule.every_nanos != interval) {
                schedule = TimerSchedule{interval, frame_time_nanos_ + interval, false};
                if (!running)
                    continue;
            }
            if (schedule.finished)
                continue;
            if (frame_time_nanos_ < schedule.due_nanos) {
                timers_waiting_ = true;
                continue;
            }
            // Fired once however many intervals passed: a stalled frame never fires a burst.
            if (repeat) {
                schedule.due_nanos += interval;
                if (schedule.due_nanos <= frame_time_nanos_)
                    schedule.due_nanos = frame_time_nanos_ + interval;
                timers_waiting_ = true;
            } else {
                schedule.finished = true;
            }
            const runtime::ActionEvent event{
                "timer",
                node->description().key,
                runtime::Value(*id->string()),
            };
            static_cast<void>(execute_action(event, **action->action(), *node, result));
        }
    }
    std::erase_if(timers_, [&declared](const auto& entry) { return !declared.contains(entry.first); });
}

runtime::ActionDispatchOutcome InputRouter::execute_composition(
    const runtime::ActionEvent& event,
    const runtime::ActionValue& composition,
    const RetainedNode& node,
    InputOperationResult& result
) {
    if (*composition.composition == runtime::ActionCompositionMode::delay) {
        if (composition.children.size() != 1U || composition.children.front() == nullptr) {
            return runtime::ActionDispatchOutcome{
                runtime::ActionDispatchStatus::failed,
                composition.action->id(),
                {"strata.surface.declarative"},
                "A delayed action needs exactly one step.",
            };
        }
        pending_delays_.push_back(PendingDelay{
            frame_time_nanos_ + composition.delay_nanoseconds,
            node.identity(),
            event,
            composition.children.front(),
        });
        if (frame_invalidator_)
            frame_invalidator_();
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::handled,
            composition.action->id(),
            {"strata.surface.declarative"},
            std::nullopt,
        };
    }
    if (*composition.composition == runtime::ActionCompositionMode::then) {
        if (composition.children.size() != 2U || composition.children[0] == nullptr ||
            composition.children[1] == nullptr) {
            return runtime::ActionDispatchOutcome{
                runtime::ActionDispatchStatus::failed,
                composition.action->id(),
                {"strata.surface.declarative"},
                "then needs a request and the action that receives its answer.",
            };
        }
        last_platform_request_.reset();
        last_answer_.reset();
        const runtime::ActionDispatchOutcome first =
            execute_action(event, *composition.children[0], node, result);
        if (first.status == runtime::ActionDispatchStatus::failed ||
            first.status == runtime::ActionDispatchStatus::unhandled) {
            last_answer_.reset();
            return first;
        }
        // An answer known at once (the clock) is `next`'s event value, as a host's answer is.
        if (last_answer_.has_value()) {
            runtime::ActionEvent answered = event;
            answered.value = std::move(*last_answer_);
            last_answer_.reset();
            return execute_action(answered, *composition.children[1], node, result);
        }
        // A platform request answers later; anything else has finished, so `next` runs now.
        if (last_platform_request_.has_value()) {
            pending_continuations_.insert_or_assign(
                *last_platform_request_,
                PendingContinuation{node.identity(), composition.children[1]});
            last_platform_request_.reset();
            return first;
        }
        return execute_action(event, *composition.children[1], node, result);
    }
    application_.undo().begin_group(public_surface_id_);
    std::vector<runtime::ActionDispatchOutcome> outcomes;
    outcomes.reserve(composition.children.size());
    for (std::size_t index = 0U; index < composition.children.size(); ++index) {
        const auto& child = composition.children[index];
        const runtime::ActionDispatchOutcome outcome = child != nullptr
            ? execute_action(event, *child, node, result)
            : runtime::ActionDispatchOutcome{
                  runtime::ActionDispatchStatus::failed,
                  std::nullopt,
                  {},
                  "Composed action contains an empty step.",
              };
        outcomes.push_back(outcome);
        if (*composition.composition == runtime::ActionCompositionMode::sequence &&
            (outcome.status == runtime::ActionDispatchStatus::failed ||
             outcome.status == runtime::ActionDispatchStatus::unhandled)) {
            const std::string id = child != nullptr && child->action != nullptr
                                       ? child->action->id()
                                       : std::string("unknown");
            application_.undo().end_group(public_surface_id_);
            return runtime::ActionDispatchOutcome{
                runtime::ActionDispatchStatus::failed,
                composition.action->id(),
                {"strata.surface.declarative"},
                "Step " + std::to_string(index + 1U) + " '" + id + "' " +
                    std::string(status_name(outcome.status)) + ": " +
                    outcome.message.value_or("no recovery detail"),
            };
        }
    }
    if (*composition.composition == runtime::ActionCompositionMode::parallel) {
        for (std::size_t index = 0U; index < outcomes.size(); ++index) {
            const auto& outcome = outcomes[index];
            if (outcome.status != runtime::ActionDispatchStatus::failed &&
                outcome.status != runtime::ActionDispatchStatus::unhandled) {
                continue;
            }
            const auto& child = composition.children[index];
            const std::string id = child != nullptr && child->action != nullptr
                                       ? child->action->id()
                                       : std::string("unknown");
            application_.undo().end_group(public_surface_id_);
            return runtime::ActionDispatchOutcome{
                runtime::ActionDispatchStatus::failed,
                composition.action->id(),
                {"strata.surface.declarative"},
                "Parallel step " + std::to_string(index + 1U) + " '" + id + "' " +
                    std::string(status_name(outcome.status)) + ": " +
                    outcome.message.value_or("no recovery detail"),
            };
        }
    }
    const bool handled = std::ranges::any_of(outcomes, [](const auto& outcome) {
        return outcome.status == runtime::ActionDispatchStatus::handled ||
               outcome.status == runtime::ActionDispatchStatus::forwarded;
    });
    application_.undo().end_group(public_surface_id_);
    return runtime::ActionDispatchOutcome{
        handled ? runtime::ActionDispatchStatus::handled : runtime::ActionDispatchStatus::ignored,
        composition.action->id(),
        {"strata.surface.declarative"},
        std::nullopt,
    };
}
} // namespace strata::ui
