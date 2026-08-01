#include "ui/render/material_registry.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace strata::ui {
namespace {

[[nodiscard]] bool finite_number(const runtime::Value& value) noexcept {
    const double* number = value.number();
    return number != nullptr && std::isfinite(*number);
}

[[nodiscard]] bool finite_vector(const runtime::Value& value, const std::size_t size) noexcept {
    const runtime::ValueList* list = value.list();
    return list != nullptr && list->values.size() == size &&
           std::ranges::all_of(list->values, finite_number);
}

[[nodiscard]] bool accepts_material_type(
    const runtime::Value& value,
    const std::optional<std::string>& type
) noexcept {
    if (!type.has_value()) return true;
    if (*type == "FLOAT") return finite_number(value);
    if (*type == "INT") {
        const double* number = value.number();
        return number != nullptr && std::isfinite(*number) && std::trunc(*number) == *number;
    }
    if (*type == "FLOAT2") return finite_vector(value, 2U);
    if (*type == "FLOAT4") return finite_vector(value, 4U);
    if (*type == "COLOR") return value.color() != nullptr;
    if (*type == "TEXTURE") {
        return value.image() != nullptr ||
               (value.string() != nullptr && !value.string()->empty());
    }
    return false;
}

[[nodiscard]] bool valid_blend_mode(const std::string_view value) noexcept {
    return value == "opaque" || value == "premultiplied_alpha" ||
           value == "straight_alpha" || value == "additive" || value == "multiply";
}

[[nodiscard]] std::string material_slot_name(const std::size_t slot) {
    return "@" + std::to_string(slot);
}

[[nodiscard]] runtime::RuntimeDiagnostic warning(
    std::string code,
    std::string message,
    std::string path,
    std::optional<std::string> expected = std::nullopt
) {
    return runtime::RuntimeDiagnostic{
        std::move(code),
        std::move(message),
        std::move(path),
        std::move(expected),
        runtime::DiagnosticSeverity::warning,
        std::nullopt,
    };
}

} // namespace

MaterialRegistry::MaterialRegistry(
    std::vector<std::string> material_ids,
    runtime::DiagnosticReporter reporter
) : material_ids_(material_ids.begin(), material_ids.end()), reporter_(std::move(reporter)) {}

MaterialRegistry::MaterialRegistry(
    const compiler::SchemaRegistry& schemas,
    runtime::DiagnosticReporter reporter
) : MaterialRegistry(schemas.material_ids(), std::move(reporter)) {
    schemas_ = &schemas;
}

void MaterialRegistry::publish_once(
    const runtime::RuntimeDiagnostic& diagnostic,
    std::string fingerprint
) const {
    if (reporter_ && published_.insert(std::move(fingerprint)).second) reporter_(diagnostic);
}

std::vector<runtime::RuntimeDiagnostic> MaterialRegistry::validate_state(
    const MaterialState& state,
    const bool report_diagnostics
) const {
    std::vector<runtime::RuntimeDiagnostic> diagnostics;
    const compiler::MaterialSchema* schema = schemas_ != nullptr ? schemas_->material(state.id) : nullptr;
    if (!material_ids_.contains(state.id)) {
        diagnostics.push_back(warning(
            "STRATA.RENDER2D.MATERIAL_UNKNOWN",
            "Material '" + state.id + "' is not registered.",
            "material/" + state.id,
            "registered material id"
        ));
    } else {
        if (!valid_blend_mode(state.blend_mode)) {
            diagnostics.push_back(warning(
                "STRATA.RENDER2D.MATERIAL_BLEND_MODE",
                "Material '" + state.id + "' uses unknown blend mode '" + state.blend_mode + "'.",
                "material/" + state.id + "/blendMode",
                "opaque, premultiplied_alpha, straight_alpha, additive, multiply"
            ));
        }
        if (!std::isfinite(state.opacity) || state.opacity < 0.0 || state.opacity > 1.0) {
            diagnostics.push_back(warning(
                "STRATA.RENDER2D.MATERIAL_OPACITY",
                "Material '" + state.id + "' opacity must be finite and between zero and one.",
                "material/" + state.id + "/opacity",
                "finite number in [0, 1]"
            ));
        }
        std::set<std::string, std::less<>> seen;
        for (const MaterialParameter& parameter : state.parameters) {
            if (!seen.insert(parameter.name).second) {
                diagnostics.push_back(warning(
                    "STRATA.RENDER2D.MATERIAL_PARAMETER_DUPLICATE",
                    "Material '" + state.id + "' supplies parameter '" + parameter.name +
                        "' more than once.",
                    "material/" + state.id + "/" + parameter.name,
                    "unique parameter name"
                ));
                continue;
            }
            if (schema == nullptr) continue;
            const compiler::SchemaParameter* expected = schema->find_parameter(parameter.name);
            if (expected == nullptr) {
                diagnostics.push_back(warning(
                    "STRATA.RENDER2D.MATERIAL_PARAMETER_UNKNOWN",
                    "Material '" + state.id + "' does not define '" + parameter.name + "'.",
                    "material/" + state.id + "/" + parameter.name,
                    "declared material parameter"
                ));
            } else if (!accepts_material_type(parameter.value, expected->material_type)) {
                diagnostics.push_back(warning(
                    "STRATA.RENDER2D.MATERIAL_PARAMETER_TYPE",
                    "Material '" + state.id + "' parameter '" + parameter.name +
                        "' expected " + expected->material_type.value_or(
                            expected->type->diagnostic_name()
                        ) + ".",
                    "material/" + state.id + "/" + parameter.name,
                    expected->material_type.value_or(expected->type->diagnostic_name())
                ));
            }
        }
    }
    if (report_diagnostics) {
        for (const runtime::RuntimeDiagnostic& diagnostic : diagnostics) {
            publish_once(diagnostic, diagnostic.code + ":" + diagnostic.path);
        }
    }
    return diagnostics;
}

std::optional<MaterialState> MaterialRegistry::sanitize_state(const MaterialState& state) const {
    static_cast<void>(validate_state(state, true));
    if (!material_ids_.contains(state.id)) return std::nullopt;
    MaterialState result = state;
    if (!valid_blend_mode(result.blend_mode)) result.blend_mode = "straight_alpha";
    if (!std::isfinite(result.opacity)) result.opacity = 1.0;
    result.opacity = std::clamp(result.opacity, 0.0, 1.0);
    if (schemas_ == nullptr) return result;
    const compiler::MaterialSchema* schema = schemas_->material(result.id);
    if (schema == nullptr) return std::nullopt;
    std::vector<MaterialParameter> parameters;
    parameters.reserve(result.parameters.size());
    std::set<std::string, std::less<>> seen;
    for (const MaterialParameter& parameter : result.parameters) {
        const compiler::SchemaParameter* expected = schema->find_parameter(parameter.name);
        if (!seen.insert(parameter.name).second || expected == nullptr ||
            !accepts_material_type(parameter.value, expected->material_type)) {
            continue;
        }
        // An authored material declares its own parameters, so the name carries no meaning past
        // this point: it is rewritten to the draw-data float the shader reads it from. Built-in
        // materials keep their names, which the shared unified shader knows by contract.
        const std::optional<std::size_t> slot = schema->shaders.empty()
            ? std::nullopt
            : schema->packing_slot(parameter.name);
        parameters.push_back(
            slot.has_value()
                ? MaterialParameter{material_slot_name(*slot), parameter.value}
                : parameter
        );
    }
    result.parameters = std::move(parameters);
    return result;
}

void MaterialRegistry::clear_diagnostics() noexcept { published_.clear(); }

} // namespace strata::ui
