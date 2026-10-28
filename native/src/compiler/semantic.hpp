#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compiler/ast.hpp"
#include "compiler/diagnostic.hpp"
#include "compiler/schema.hpp"
#include "compiler/semantic/model.hpp"

namespace strata::compiler {

enum class ValidatedAnimationValueKind { number, boolean, color };

struct ValidatedAnimationValue final {
    ValidatedAnimationValueKind kind = ValidatedAnimationValueKind::number;
    double number = 0.0;
    bool boolean = false;
    std::string color_rgba;
};

struct ValidatedAnimationTrack final {
    std::string property;
    ValidatedAnimationValue from;
    ValidatedAnimationValue to;
};

enum class ValidatedAnimationRepeatKind { none, forever, count };

struct ValidatedAnimationRepeat final {
    ValidatedAnimationRepeatKind kind = ValidatedAnimationRepeatKind::none;
    std::uint32_t count = 1U;
};

/** Fully checked v1 animation data. Portable lowering is a projection, never a recovery pass. */
struct ValidatedAnimation final {
    std::string name;
    std::vector<ValidatedAnimationTrack> tracks;
    std::int64_t duration_nanos = 300'000'000;
    std::int64_t delay_nanos = 0;
    std::string easing = "linear";
    std::string fill_mode = "both";
    ValidatedAnimationRepeat repeat;
    bool reverse = false;
    std::string trigger = "ANIMATE";
};

/**
 * Tokens `theme.<name>` can read, as ui::ThemeTokens resolves them; a runtime test keeps the two in
 * step.
 */
inline constexpr std::array<std::string_view, 7U> theme_color_tokens{
    "surface", "surfaceRaised", "foreground", "mutedForeground", "accent", "danger", "focus",
};
inline constexpr std::array<std::string_view, 3U> theme_number_tokens{
    "spacingUnit",
    "radius",
    "density",
};
/** Host roots every surface supplies itself, without an application schema declaring them. */
inline constexpr std::array<std::string_view, 1U> contextual_host_roots{"env"};

/** Items an eagerly expanded loop may draw, and so the bound of a declared or retained list. */
inline constexpr std::size_t maximum_eager_loop_items = 1'000U;

/**
 * The type an authored type reference names. Analysis resolves every declared type through here,
 * so a declaration means one thing wherever it is read. A name the registry does not define resolves
 * to `unknown` and is appended to `unresolved`, when given, for the caller to report.
 */
[[nodiscard]] SemanticTypePtr resolve_type_reference(
    const TypeReference& reference,
    const SchemaRegistry& registry,
    std::vector<const TypeReference*>* unresolved = nullptr
);

/** A component parameter's type; an untyped parameter needs an explicit schema to be passed. */
[[nodiscard]] SemanticTypePtr resolve_parameter_type(
    const Parameter& parameter,
    const SchemaRegistry& registry,
    std::vector<const TypeReference*>* unresolved = nullptr
);

struct SemanticResult final {
    std::vector<Diagnostic> diagnostics;
    std::map<std::string, ValidatedAnimation, std::less<>> animations;
    /**
     * The resolved, typed module. It borrows the analysed file's nodes, so the file must outlive
     * it; lowering and the source map project it.
     */
    SemanticModel model;
};

/** Whether every module parsed and loaded, so every declaration an author wrote is present. */
enum class AnalysisInput { complete, incomplete };

/**
 * Analyses a module in passes: collect declarations, resolve names, type check, policy lints and
 * placement. It runs on files with syntax errors too: a node the parser could not read types as
 * unknown and draws no further diagnostics, and with `incomplete` input a name that resolves to
 * nothing is not reported, since its declaration may be what failed to parse or load.
 */
[[nodiscard]] SemanticResult validate_semantics(
    const File& file,
    const SchemaRegistry& registry,
    AnalysisInput input = AnalysisInput::complete
);

} // namespace strata::compiler
