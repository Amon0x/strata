#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "compiler/semantic/analysis.hpp"

// Animation declarations check into ValidatedAnimation, the literal data lowering emits.
namespace strata::compiler::semantic {
namespace {

class AnimationChecker final {
public:
    using ValidateExpected =
        std::function<void(const Expression&, const SemanticType&, bool, const std::string&)>;
    using CheckValue = std::function<void(const Expression&, const std::string&)>;

    AnimationChecker(Analysis& analysis, const ValidateExpected& validate_expected,
                     const CheckValue& check_value)
        : analysis_(analysis), validate_expected_(validate_expected), check_value_(check_value) {}

    void run(const AnimationDeclaration& animation, const SourceSpan& declaration_span) {
        const SchemaRegistry& registry = analysis_.registry;
        const std::string path = "animation " + animation.name;
        const std::size_t diagnostic_start = analysis_.diagnostics.size();
        std::vector<const AnimationFrame*> from_frames;
        std::vector<const AnimationFrame*> to_frames;
        std::map<std::string, const Property*, std::less<>> timing;
        std::map<const Property*, std::optional<ValidatedAnimationValue>> frame_literals;

        for (const AnimationEntry& entry : animation.entries) {
            if (const auto* timing_entry = std::get_if<AnimationProperty>(&entry)) {
                const Property& property = timing_entry->property;
                if (!timing.emplace(property.name, &property).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                           "Animation timing property '" + property.name +
                               "' is declared more than once.",
                           property.span, path, "unique animation timing property");
                }
                const SchemaParameter* schema = registry.animation_timing_property(property.name);
                if (schema == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                           "Animation timing property '" + property.name + "' is not supported." +
                               did_you_mean(property.name,
                                            registry.animation_timing_property_names()),
                           property.span, path,
                           expected_names(registry.animation_timing_property_names()));
                    check_value_(*property.value, path + "." + property.name);
                } else {
                    validate_expected_(*property.value, *schema->type, schema->nullable,
                                       path + "." + property.name);
                }
                continue;
            }
            const AnimationFrame& frame = std::get<AnimationFrame>(entry);
            auto& frames = frame.phase == AnimationFramePhase::from ? from_frames : to_frames;
            frames.push_back(&frame);
            if (frames.size() > 1U) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_ANIMATION_FRAME",
                       "Animation '" + animation.name + "' declares more than one " +
                           (frame.phase == AnimationFramePhase::from ? "from" : "to") + " frame.",
                       frame.span, path, "exactly one from frame and one to frame");
            }
            const std::string frame_path =
                path + (frame.phase == AnimationFramePhase::from ? "/from" : "/to");
            std::set<std::string, std::less<>> property_names;
            for (const Property& property : frame.properties) {
                if (!property_names.insert(property.name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                           "Animation frame property '" + property.name +
                               "' is declared more than once.",
                           property.span, frame_path, "unique animation frame property");
                }
                const SchemaParameter* schema = registry.animation_property(property.name);
                if (schema == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                           "Animation frame property '" + property.name + "' is not supported." +
                               did_you_mean(property.name, registry.animation_property_names()),
                           property.span, frame_path,
                           expected_names(registry.animation_property_names()));
                    check_value_(*property.value, frame_path + "." + property.name);
                } else {
                    validate_expected_(*property.value, *schema->type, false,
                                       frame_path + "." + property.name);
                    frame_literals.emplace(&property, frame_literal(property, frame_path));
                }
            }
        }

        if (from_frames.empty() && to_frames.empty()) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_NO_KEYFRAMES",
                   "Animation '" + animation.name +
                       "' must declare one from and one to keyframe block.",
                   declaration_span, path, "one from and one to keyframe block");
        } else if (from_frames.size() != 1U || to_frames.size() != 1U) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_FRAME_PAIR",
                   "Animation '" + animation.name +
                       "' must declare exactly one from frame and exactly one to frame.",
                   declaration_span, path, "exactly one from frame and one to frame");
        }

        std::map<std::string, const Property*, std::less<>> from_properties;
        std::map<std::string, const Property*, std::less<>> to_properties;
        if (from_frames.size() == 1U) {
            for (const Property& property : from_frames.front()->properties)
                from_properties.emplace(property.name, &property);
        }
        if (to_frames.size() == 1U) {
            for (const Property& property : to_frames.front()->properties)
                to_properties.emplace(property.name, &property);
        }
        const bool paired = from_frames.size() == 1U && to_frames.size() == 1U;
        if (paired && from_properties.empty() && to_properties.empty()) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_NO_TRACKS",
                   "Animation '" + animation.name + "' must animate at least one property.",
                   declaration_span, path, "matching non-empty frame property sets");
        } else if (paired) {
            std::vector<std::string> from_names;
            std::vector<std::string> to_names;
            for (const auto& [name, property] : from_properties) {
                static_cast<void>(property);
                from_names.push_back(name);
            }
            for (const auto& [name, property] : to_properties) {
                static_cast<void>(property);
                to_names.push_back(name);
            }
            if (from_names != to_names) {
                report("STRATA.DSL.SEMANTIC_ANIMATION_FRAME_MISMATCH",
                       "Animation '" + animation.name +
                           "' must declare the same property set in its from and to frames.",
                       to_frames.front()->span, path + "/to", expected_names(std::move(from_names)));
            }
        }

        ValidatedAnimation validated;
        validated.name = animation.name;
        const auto timing_property = [&timing](const std::string_view name) -> const Property* {
            const auto found = timing.find(name);
            return found != timing.end() ? found->second : nullptr;
        };
        if (const Property* property = timing_property("duration")) {
            if (const auto value = duration(*property, path + ".duration", true))
                validated.duration_nanos = *value;
        }
        if (const Property* property = timing_property("delay")) {
            if (const auto value = duration(*property, path + ".delay", false))
                validated.delay_nanos = *value;
        }
        if (const Property* property = timing_property("easing")) {
            if (const auto value =
                    string_literal(*property, path + ".easing",
                                   {"cubic-in", "cubic-in-out", "cubic-out", "ease", "ease-in",
                                    "ease-out", "linear"})) {
                validated.easing = *value;
            }
        }
        if (const Property* property = timing_property("fillMode")) {
            if (const auto value = string_literal(*property, path + ".fillMode",
                                                  {"BACKWARDS", "BOTH", "FORWARDS", "NONE"})) {
                validated.fill_mode = lower(*value);
            }
        }
        if (const Property* property = timing_property("trigger")) {
            if (const auto value =
                    string_literal(*property, path + ".trigger",
                                   {"ANIMATE", "CHECKED", "ENTER", "EXIT", "FOCUS", "FOCUS_VISIBLE",
                                    "HOVER", "MOVE", "PRESSED"})) {
                validated.trigger = *value;
            }
        }
        if (const Property* property = timing_property("reverse")) {
            const LiteralExpression* literal = static_literal(*property->value);
            const auto* boolean =
                literal != nullptr ? std::get_if<BooleanLiteral>(&literal->value) : nullptr;
            if (boolean == nullptr) {
                report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                       "Animation timing property 'reverse' must be a boolean literal.",
                       property->value->span, path + ".reverse", "boolean literal");
            } else {
                validated.reverse = boolean->value;
            }
        }
        if (const Property* property = timing_property("repeat")) {
            if (const auto value = repeat(*property, path + ".repeat")) validated.repeat = *value;
        }

        if (paired && from_properties.size() == to_properties.size()) {
            for (const Property& from : from_frames.front()->properties) {
                const auto matching = to_properties.find(from.name);
                if (matching == to_properties.end()) continue;
                const auto from_value = frame_literals.find(&from);
                const auto to_value = frame_literals.find(matching->second);
                if (from_value != frame_literals.end() && from_value->second.has_value() &&
                    to_value != frame_literals.end() && to_value->second.has_value()) {
                    validated.tracks.push_back(ValidatedAnimationTrack{
                        from.name,
                        *from_value->second,
                        *to_value->second,
                    });
                }
            }
        }
        if (analysis_.diagnostics.size() == diagnostic_start)
            analysis_.animations.try_emplace(animation.name, std::move(validated));
    }

private:
    void report(std::string code, std::string message, const SourceSpan& span, std::string path,
                std::string expected) {
        analysis_.diagnostics.report(std::move(code), std::move(message), span, std::move(path),
                                     std::move(expected));
    }

    [[nodiscard]] std::optional<ValidatedAnimationValue> frame_literal(const Property& property,
                                                                       const std::string& path) {
        if (const std::optional<StaticNumber> number = static_number(*property.value);
            number.has_value() && !number->unit.has_value()) {
            return ValidatedAnimationValue{ValidatedAnimationValueKind::number, number->value,
                                           false, {}};
        }
        if (const LiteralExpression* literal = static_literal(*property.value)) {
            if (const auto* boolean = std::get_if<BooleanLiteral>(&literal->value)) {
                return ValidatedAnimationValue{ValidatedAnimationValueKind::boolean, 0.0,
                                               boolean->value, {}};
            }
            if (const auto* color = std::get_if<ColorLiteral>(&literal->value)) {
                return ValidatedAnimationValue{ValidatedAnimationValueKind::color, 0.0, false,
                                               canonical_color(color->raw)};
            }
        }
        report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
               "Animation frame property '" + property.name +
                   "' must be a number, boolean, or color literal.",
               property.value->span, path, "literal animation value");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t> duration(const Property& property,
                                                       const std::string& path,
                                                       const bool positive) {
        const std::optional<StaticNumber> number = static_number(*property.value);
        if (!number.has_value() || !number->unit.has_value()) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                   "Animation timing property '" + property.name + "' must be a duration literal.",
                   property.value->span, path, "duration literal");
            return std::nullopt;
        }
        const long double scale = *number->unit == "ms"  ? 1'000'000.0L
                                  : *number->unit == "s" ? 1'000'000'000.0L
                                                         : 0.0L;
        const long double nanos = static_cast<long double>(number->value) * scale;
        const long double rounded_nanos = std::round(nanos);
        const long double rounding_tolerance =
            std::numeric_limits<long double>::epsilon() * std::max(1.0L, std::abs(nanos)) * 8.0L;
        const bool in_range =
            scale != 0.0L && std::isfinite(nanos) &&
            rounded_nanos <= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) &&
            rounded_nanos >= 0.0L && std::abs(nanos - rounded_nanos) <= rounding_tolerance &&
            (!positive || rounded_nanos >= 1.0L);
        if (!in_range) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_DURATION_BOUNDS",
                   "Animation timing property '" + property.name + "' must resolve to " +
                       (positive ? "a positive" : "a non-negative") +
                       " whole-nanosecond duration within int64 bounds.",
                   property.value->span, path,
                   positive ? "positive duration" : "non-negative duration");
            return std::nullopt;
        }
        return static_cast<std::int64_t>(rounded_nanos);
    }

    [[nodiscard]] std::optional<std::string> string_literal(
        const Property& property, const std::string& path,
        const std::set<std::string, std::less<>>& allowed) {
        const LiteralExpression* literal = static_literal(*property.value);
        const auto* string =
            literal != nullptr ? std::get_if<StringLiteral>(&literal->value) : nullptr;
        const std::string allowed_names =
            expected_names(std::vector<std::string>(allowed.begin(), allowed.end()));
        if (string == nullptr) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                   "Animation timing property '" + property.name + "' must be a string literal.",
                   property.value->span, path, allowed_names);
            return std::nullopt;
        }
        if (!allowed.contains(string->value)) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_SCHEMA_VALUE",
                   "Animation timing property '" + property.name + "' has unsupported value '" +
                       string->value + "'.",
                   property.value->span, path, allowed_names);
            return std::nullopt;
        }
        return string->value;
    }

    [[nodiscard]] std::optional<ValidatedAnimationRepeat> repeat(const Property& property,
                                                                 const std::string& path) {
        const Expression& expression = ungrouped(*property.value);
        if (const LiteralExpression* literal = static_literal(expression); literal != nullptr) {
            if (const auto* string = std::get_if<StringLiteral>(&literal->value)) {
                if (string->value == "none") return ValidatedAnimationRepeat{};
                if (string->value == "forever")
                    return ValidatedAnimationRepeat{ValidatedAnimationRepeatKind::forever, 1U};
                report("STRATA.DSL.SEMANTIC_ANIMATION_SCHEMA_VALUE",
                       "Animation repeat string must be exactly 'none' or 'forever'.",
                       property.value->span, path, "none, forever, or a positive integral count");
                return std::nullopt;
            }
        }
        const Expression* count_expression = &expression;
        if (const auto* map = std::get_if<MapExpression>(&expression.node); map != nullptr) {
            if (map->entries.size() != 1U || static_map_key(map->entries.front().key) == nullptr ||
                *static_map_key(map->entries.front().key) != "count") {
                report("STRATA.DSL.SEMANTIC_ANIMATION_REPEAT_COUNT",
                       "Animation repeat maps must contain exactly one literal 'count' field.",
                       property.value->span, path, "{ count: positive integer }");
                return std::nullopt;
            }
            count_expression = map->entries.front().value.get();
        }
        const std::optional<StaticNumber> count = static_number(*count_expression);
        constexpr double maximum = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
        if (!count.has_value() || count->unit.has_value() || count->value <= 0.0 ||
            count->value > maximum || std::floor(count->value) != count->value) {
            report(count.has_value() ? "STRATA.DSL.SEMANTIC_ANIMATION_REPEAT_COUNT"
                                     : "STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                   count.has_value()
                       ? "Animation repeat count must be a positive integral uint32 value."
                       : "Animation repeat must be 'none', 'forever', a positive integer, or a "
                         "literal count map.",
                   count_expression->span, path, "positive integral uint32 repeat count");
            return std::nullopt;
        }
        return ValidatedAnimationRepeat{ValidatedAnimationRepeatKind::count,
                                        static_cast<std::uint32_t>(count->value)};
    }

    Analysis& analysis_;
    const ValidateExpected& validate_expected_;
    const CheckValue& check_value_;
};

} // namespace

void check_animation(
    Analysis& analysis,
    const AnimationDeclaration& animation,
    const SourceSpan& declaration_span,
    const std::function<void(const Expression&, const SemanticType&, bool, const std::string&)>&
        validate_expected,
    const std::function<void(const Expression&, const std::string&)>& check_value
) {
    AnimationChecker(analysis, validate_expected, check_value).run(animation, declaration_span);
}

} // namespace strata::compiler::semantic
