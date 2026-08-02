#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "compiler/schema.hpp"
#include "runtime/diagnostic.hpp"
#include "ui/render.hpp"

namespace strata::ui {

/** Instance-owned material contract registry used by render planning and host validation. */
class MaterialRegistry final {
public:
    explicit MaterialRegistry(
        std::vector<std::string> material_ids = {},
        runtime::DiagnosticReporter reporter = {}
    );
    MaterialRegistry(
        const compiler::SchemaRegistry& schemas,
        runtime::DiagnosticReporter reporter = {}
    );

    [[nodiscard]] std::vector<runtime::RuntimeDiagnostic> validate_state(
        const MaterialState& state,
        bool report_diagnostics = true
    ) const;
    /** Returns packet-safe state, dropping invalid fields; an unknown material has no safe state. */
    [[nodiscard]] std::optional<MaterialState> sanitize_state(const MaterialState& state) const;
    [[nodiscard]] std::vector<runtime::RuntimeDiagnostic> validate_effect_state(
        const EffectState& state,
        bool report_diagnostics = true
    ) const;
    /** Returns packet-safe effect state with its declared input and valid parameters only. */
    [[nodiscard]] std::optional<EffectState> sanitize_effect_state(
        const EffectState& state
    ) const;
    /** Rearms once-per-fingerprint warning publication after the diagnostic store is cleared. */
    void clear_diagnostics() noexcept;

private:
    void publish_once(const runtime::RuntimeDiagnostic& diagnostic, std::string fingerprint) const;

    const compiler::SchemaRegistry* schemas_ = nullptr;
    std::set<std::string, std::less<>> material_ids_;
    runtime::DiagnosticReporter reporter_;
    mutable std::set<std::string, std::less<>> published_;
};

} // namespace strata::ui
