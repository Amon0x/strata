#include "ui/input.hpp"

#include <algorithm>
#include <cctype>
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
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;

void InputRouter::report_action_outcome(
    const runtime::Action& action,
    const runtime::ActionDispatchOutcome& outcome,
    const RetainedNode& node
) {
    if (outcome.status != runtime::ActionDispatchStatus::unhandled &&
        outcome.status != runtime::ActionDispatchStatus::failed) {
        return;
    }

    std::string layer = "root";
    for (const RetainedNode* current = &node; current != nullptr; current = current->parent()) {
        if (current->description().type != "SurfaceLayer" ||
            !current->description().key.has_value()) {
            continue;
        }
        constexpr std::string_view prefix = "strata.layer.";
        const std::string& key = *current->description().key;
        layer = key.starts_with(prefix) ? key.substr(prefix.size()) : key;
        break;
    }

    const std::string source_key = node.description().key.value_or("unkeyed");
    std::string message = outcome.message.value_or(
        "Action dispatch " + std::string(status_name(outcome.status)) + "."
    );
    message += " Source node=" + source_key + ", type=" + node.description().type +
               ", surface=" + public_surface_id_ + ", layer=" + layer + ".";
    message += " Payload contract=" + action.contract->payload_contract + ".";

    std::optional<runtime::DiagnosticRange> range;
    std::string component_path(node.structural_path());
    if (action.origin.has_value()) {
        const runtime::ActionOrigin& origin_value = *action.origin;
        if (origin_value.component_path.has_value()) component_path = *origin_value.component_path;
        if (origin_value.line.has_value()) {
            const runtime::DiagnosticPosition start{
                *origin_value.line, origin_value.column.value_or(1U), std::nullopt,
            };
            const runtime::DiagnosticPosition end = origin_value.end_line.has_value()
                ? runtime::DiagnosticPosition{
                      *origin_value.end_line, origin_value.end_column.value_or(1U), std::nullopt,
                  }
                : start;
            range = runtime::DiagnosticRange{origin_value.source_id, start, end};
        }
    }

    pending_diagnostics_.push_back(runtime::RuntimeDiagnostic{
        outcome.status == runtime::ActionDispatchStatus::unhandled
            ? "STRATA.UI.ACTION_UNHANDLED"
            : "STRATA.UI.ACTION_DISPATCH_FAILED",
        std::move(message),
        std::move(component_path),
        outcome.status == runtime::ActionDispatchStatus::unhandled
            ? std::optional<std::string>("registered handler or explicit ignore/forward policy")
            : std::optional<std::string>(
                  "payload satisfying the active action contract and a successful handler"
              ),
        runtime::DiagnosticSeverity::error,
        std::move(range),
    });
}

std::vector<runtime::RuntimeDiagnostic> InputRouter::take_diagnostics() {
    std::vector<runtime::RuntimeDiagnostic> diagnostics = std::move(pending_diagnostics_);
    pending_diagnostics_.clear();
    return diagnostics;
}

void InputRouter::clear_diagnostics() noexcept { pending_diagnostics_.clear(); }
} // namespace strata::ui
