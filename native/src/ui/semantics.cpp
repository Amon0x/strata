#include "ui/semantics.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/input.hpp"
#include "ui/widget/semantics.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] bool has_semantics_property(const RetainedNode& node) {
    const auto found = node.description().properties.find("semantics");
    return found != node.description().properties.end() && found->second.value() != nullptr;
}

[[nodiscard]] bool participates(
    const RetainedNode& node,
    const WidgetRegistry& widgets
) {
    const WidgetLifecycle* lifecycle = widgets.find(node.description().type);
    return lifecycle == nullptr || !lifecycle->participates || lifecycle->participates(node);
}

[[nodiscard]] bool focusable(
    const RetainedNode& node,
    const WidgetRegistry& widgets,
    const BehaviorRegistry& behaviors
) {
    if (!participates(node, widgets)) return false;
    const WidgetLifecycle* lifecycle = widgets.find(node.description().type);
    bool result = lifecycle != nullptr && lifecycle->input.focusable;
    if (result && !lifecycle->input.focusable_when.empty()) {
        const auto condition = node.description().properties.find(lifecycle->input.focusable_when);
        const runtime::Value* value = condition != node.description().properties.end()
                                          ? condition->second.value()
                                          : nullptr;
        result = value != nullptr && value->boolean() != nullptr && *value->boolean();
    }
    for (const DescriptionBehavior& attachment : node.description().behaviors) {
        if (!attachment.enabled) continue;
        const BehaviorLifecycle* behavior = behaviors.find(attachment.id);
        if (behavior != nullptr) result = result || behavior->input.focusable;
    }
    return result;
}

[[nodiscard]] std::string node_description(const RetainedNode& node) {
    return node.description().type +
           (node.description().key.has_value() ? "#" + *node.description().key : std::string{});
}

} // namespace

SemanticsEngine::SemanticsEngine(
    const WidgetRegistry& widgets,
    const BehaviorRegistry& behaviors
) : widgets_(widgets), behaviors_(behaviors) {}

JsonValue SemanticsEngine::build(const RetainedNode& node) const {
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    WidgetSemanticsScope scope(
        node,
        commands_,
        input_,
        lifecycle != nullptr ? lifecycle->semantics.role : std::string("group")
    );
    if (lifecycle != nullptr && !lifecycle->semantics.actions.empty()) {
        scope.actions(lifecycle->semantics.actions);
    }
    if (lifecycle != nullptr && lifecycle->semantics.derive != nullptr) {
        lifecycle->semantics.derive(scope);
    } else {
        if (const auto label = scope.text(scope.property("label")); label.has_value()) {
            scope.default_name(*label);
        } else if (node.description().key.has_value()) {
            scope.default_name(*node.description().key);
        }
    }
    const bool node_focusable = focusable(node, widgets_, behaviors_);
    bool disabled = false;
    for (const DescriptionBehavior& attachment : node.description().behaviors) {
        if (!attachment.enabled) continue;
        const BehaviorLifecycle* behavior = behaviors_.find(attachment.id);
        if (behavior == nullptr) continue;
        disabled = disabled || behavior->input.disabled;
    }
    scope.input_capabilities(node_focusable, disabled);

    JsonValue::Array children;
    children.reserve(node.children().size());
    append_children(node, children);
    JsonValue result = scope.build(std::move(children));
    nodes_.insert_or_assign(node.identity(), result);
    return result;
}

void SemanticsEngine::append_children(
    const RetainedNode& node,
    JsonValue::Array& children
) const {
    for (const auto& child : node.children()) {
        const WidgetLifecycle* lifecycle = widgets_.find(child->description().type);
        const bool hidden = widget_semantic_hidden(*child) ||
                            (lifecycle != nullptr && lifecycle->semantics.hidden) ||
                            !participates(*child, widgets_);
        if (hidden) continue;
        const bool transparent = lifecycle != nullptr &&
                                 lifecycle->semantics.transparent_when_single_child &&
                                 !child->description().key.has_value() &&
                                 child->description().behaviors.empty() &&
                                 child->children().size() == 1U &&
                                 !has_semantics_property(*child);
        if (transparent) append_children(*child, children);
        else children.push_back(build(*child));
    }
}

bool SemanticsEngine::update(
    const RetainedTree& tree,
    const CommandIndex& commands,
    const InputRouter& input,
    const std::string_view surface_id
) {
    static_cast<void>(surface_id);
    commands_ = &commands;
    input_ = &input;
    tree_ = &tree;
    const std::optional<std::uint64_t> root_identity = tree.root() != nullptr
                                                           ? std::optional(tree.root()->identity())
                                                           : std::nullopt;
    const DirtyGenerationSnapshot all_generations = tree.dirty_generations();
    const DirtyGenerationSnapshot dirty_generations{
        .structure = all_generations.structure,
        .properties = all_generations.properties,
        .text = all_generations.text,
        .semantics = all_generations.semantics,
        .input = all_generations.input,
        .editor = all_generations.editor,
    };
    if (root_identity_ == root_identity && dirty_generations_ == dirty_generations) return false;
    if (generation_ == std::numeric_limits<std::uint64_t>::max() ||
        publish_count_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("semantic snapshot generation exhausted");
    }
    root_identity_ = root_identity;
    dirty_generations_ = dirty_generations;
    ++generation_;
    ++publish_count_;
    return true;
}

void SemanticsEngine::materialize() const {
    if (materialized_generation_ == generation_) return;
    nodes_.clear();
    const bool root_participates = tree_ != nullptr && tree_->root() != nullptr &&
                                   participates(*tree_->root(), widgets_);
    root_ = root_participates && !widget_semantic_hidden(*tree_->root())
        ? build(*tree_->root())
        : JsonValue{};
    if (tree_ != nullptr && tree_->root() != nullptr) {
        const auto validate = [this](auto&& self, const RetainedNode& node) -> void {
            if (!participates(node, widgets_)) return;
            if (focusable(node, widgets_, behaviors_) && !nodes_.contains(node.identity())) {
                const std::string fingerprint =
                    "STRATA.UI.SEMANTICS_FOCUSABLE_INVISIBLE:" +
                    std::string(node.structural_path());
                if (reported_diagnostics_.insert(fingerprint).second) {
                    diagnostics_.push_back(runtime::RuntimeDiagnostic{
                        "STRATA.UI.SEMANTICS_FOCUSABLE_INVISIBLE",
                        "Focusable node " + node_description(node) +
                            " is hidden from the semantics tree.",
                        node.description().source_path,
                        std::nullopt,
                        runtime::DiagnosticSeverity::error,
                        std::nullopt,
                    });
                }
            }
            for (const auto& child : node.children()) self(self, *child);
        };
        validate(validate, *tree_->root());
    }
    materialized_generation_ = generation_;
}

const JsonValue* SemanticsEngine::find(const std::uint64_t identity) const {
    materialize();
    const auto found = nodes_.find(identity);
    return found != nodes_.end() ? &found->second : nullptr;
}

JsonValue SemanticsEngine::snapshot(const std::string_view surface_id) const {
    materialize();
    return object({
        {"generation", JsonValue(static_cast<std::int64_t>(generation_))},
        {"publishCount", JsonValue(static_cast<std::int64_t>(publish_count_))},
        {"root", root_},
        {"surfaceId", JsonValue(std::string(surface_id))},
    });
}

std::uint64_t SemanticsEngine::generation() const noexcept { return generation_; }
std::uint64_t SemanticsEngine::publish_count() const noexcept { return publish_count_; }

std::vector<runtime::RuntimeDiagnostic> SemanticsEngine::take_diagnostics() {
    std::vector<runtime::RuntimeDiagnostic> result = std::move(diagnostics_);
    diagnostics_.clear();
    return result;
}

void SemanticsEngine::clear_diagnostics() noexcept {
    diagnostics_.clear();
    reported_diagnostics_.clear();
}

void SemanticsEngine::clear() noexcept {
    generation_ = 0U;
    publish_count_ = 0U;
    root_identity_.reset();
    dirty_generations_.reset();
    tree_ = nullptr;
    commands_ = nullptr;
    input_ = nullptr;
    materialized_generation_ = 0U;
    root_ = JsonValue{};
    nodes_.clear();
    diagnostics_.clear();
    reported_diagnostics_.clear();
}

} // namespace strata::ui
