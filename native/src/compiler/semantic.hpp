#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
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

struct SemanticResult final {
    std::vector<Diagnostic> diagnostics;
    std::vector<Diagnostic> lowering_diagnostics;
    std::map<std::string, ValidatedAnimation, std::less<>> animations;
};

[[nodiscard]] SemanticResult validate_semantics(const File& file, const SchemaRegistry& registry);

} // namespace strata::compiler
