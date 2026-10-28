#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compiler/ast.hpp"
#include "compiler/diagnostic.hpp"
#include "compiler/schema.hpp"

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

struct SemanticResult final {
    std::vector<Diagnostic> diagnostics;
    std::map<std::string, ValidatedAnimation, std::less<>> animations;
};

/** What a bare name in an expression denotes. */
enum class NameBinding { local, host, component, style, animation, unresolved };

/** The declarations of a module a bare name can refer to. */
struct ModuleNames final {
    explicit ModuleNames(const File& file);

    /**
     * What `name` denotes, where `local` says whether a local value or parameter of that name is in
     * scope. Semantic analysis and lowering both resolve names here, so a name is typed and lowered
     * as the same thing: locals shadow host roots, which shadow the module's declarations.
     */
    [[nodiscard]] NameBinding resolve(
        std::string_view name,
        bool local,
        const SchemaRegistry& registry
    ) const;

    std::set<std::string, std::less<>> components;
    std::set<std::string, std::less<>> styles;
    std::set<std::string, std::less<>> animations;
};

/** Items an eagerly expanded loop may draw, and so the bound of a declared or retained list. */
inline constexpr std::size_t maximum_eager_loop_items = 1'000U;

/**
 * The type an authored type reference names. Semantic analysis and lowering both resolve through
 * here so a declaration means the same thing to each. A name the registry does not define resolves
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

[[nodiscard]] SemanticResult validate_semantics(const File& file, const SchemaRegistry& registry);

} // namespace strata::compiler
