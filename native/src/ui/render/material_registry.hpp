#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "compiler/schema.hpp"
#include "runtime/diagnostic.hpp"
#include "ui/render.hpp"

namespace strata::ui {

/**
 * How far outside its bounds, in logical pixels, an effect's source and backdrop stay defined: the
 * HLSL prelude's effectSampleReach. The C ABI pins it to STRATA_EFFECT_SAMPLE_REACH.
 */
inline constexpr double effect_sample_reach = 64.0;

/**
 * How far outside its bounds, in logical pixels, an effect's output depends on what was drawn
 * before it. Authored shader passes may sample up to effect_sample_reach; a blur reads its
 * radius around the bounds. Passes write inside the bounds only, so the widest pass decides. Two
 * pixels of slack cover rounding the bounds and the blur padding to device pixels.
 */
[[nodiscard]] double effect_read_reach(bool shader_passes, double widest_blur_radius) noexcept;

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
