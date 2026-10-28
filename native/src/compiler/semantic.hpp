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

/** Whether every import loaded, so every module's declarations are in the analysed file. */
enum class AnalysisInput { complete, missing_imports };

/**
 * Analyses a module in passes: collect declarations, resolve names, type check, policy lints and
 * placement. It runs on files with syntax errors too: a node the parser could not read types as
 * unknown and draws no further diagnostics. A name that resolves to nothing is not reported when a
 * missing declaration could explain it: a name read in a region the parser abandoned (see
 * `File::unparsed_names`), any name past a parse limit, or, with `missing_imports`, a component,
 * style, animation or state name, which a module that failed to load could have declared. Every
 * other unresolved name is still reported.
 */
[[nodiscard]] SemanticResult validate_semantics(
    const File& file,
    const SchemaRegistry& registry,
    AnalysisInput input = AnalysisInput::complete
);

} // namespace strata::compiler
