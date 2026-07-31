#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/artifact.hpp"
#include "compiler/lexer.hpp"
#include "compiler/parser.hpp"
#include "compiler/portable_ir.hpp"
#include "compiler/source.hpp"
#include "resource/resource.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

bool has_diagnostic(
    const strata::compiler::ParseResult& result,
    const std::string_view code
) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

void test_utf8_source_positions_use_kotlin_compatible_utf16_offsets() {
    const strata::compiler::SourceBuffer source("unicode.strata", "\xF0\x9F\x98\x80x\n\xC4\xB0");
    const auto emoji = source.span(0U, 4U);
    check(
        emoji.start == strata::compiler::SourcePosition{1U, 1U, 0U} &&
            emoji.end == strata::compiler::SourcePosition{1U, 3U, 2U} && emoji.length == 2U,
        "supplementary code point must occupy two UTF-16 source units"
    );
    const auto second_line = source.span(6U, 8U);
    check(
        second_line.start == strata::compiler::SourcePosition{2U, 1U, 4U} &&
            second_line.end == strata::compiler::SourcePosition{2U, 2U, 5U},
        "UTF-8 line/column mapping changed"
    );
}

void test_lexer_contract() {
    const std::string source =
        "import \"app/lib.strata\";\n"
        "screen Main { state count = 1_000; root Text(text: #AABBCCFF) }";
    const auto result = strata::compiler::Lexer("app/main.strata", source).lex();
    check(result.diagnostics.empty(), "valid lexical fixture produced diagnostics");
    check(result.tokens.front().type == strata::compiler::TokenType::import_keyword, "import keyword changed");
    check(result.tokens.back().type == strata::compiler::TokenType::end_of_file, "EOF token missing");
    bool found_number = false;
    bool found_color = false;
    for (const auto& token : result.tokens) {
        found_number = found_number ||
                       (token.type == strata::compiler::TokenType::number && token.lexeme == "1_000");
        found_color = found_color ||
                      (token.type == strata::compiler::TokenType::color && token.lexeme == "#AABBCCFF");
    }
    check(found_number && found_color, "number/color tokenization changed");
}

void test_recovery_diagnostics() {
    const auto result = strata::compiler::Lexer(
        "bad.strata",
        "state value = 12wat;\n@\n/* open"
    ).lex();
    check(result.diagnostics.size() == 3U, "lexer recovery diagnostic count changed");
    check(
        result.diagnostics[0].code == "STRATA.DSL.LEX_INVALID_NUMBER_SUFFIX" &&
            result.diagnostics[0].range->start.offset == 14U &&
            result.diagnostics[0].range->end.offset == 19U,
        "invalid number suffix diagnostic changed"
    );
    check(
        result.diagnostics[1].code == "STRATA.DSL.LEX_UNEXPECTED_CHAR" &&
            result.diagnostics[1].message == "Unexpected character '@'.",
        "unexpected character diagnostic changed"
    );
    check(
        result.diagnostics[2].code == "STRATA.DSL.LEX_UNTERMINATED_COMMENT",
        "unterminated comment diagnostic changed"
    );
}

void test_parser_and_security_recovery_match_the_corpus_contract() {
    const std::string source =
        "/* Runtime.getRuntime is deliberately prohibited in source, including comments. */\n"
        "screen Broken {\n"
        "  state value = 12wat;\n"
        "  root Panel {\n"
        "    Text(text \"missing colon\")\n"
        "    @\n"
        "  }\n"
        "}\n";
    const auto result = strata::compiler::parse_source("app/main.strata", source);
    check(result.diagnostics.size() == 5U, "syntax/security recovery diagnostic count changed");
    check(
        result.diagnostics[0].code == "STRATA.DSL.LEX_INVALID_NUMBER_SUFFIX" &&
            result.diagnostics[1].code == "STRATA.DSL.LEX_UNEXPECTED_CHAR" &&
            result.diagnostics[2].code == "STRATA.DSL.SECURITY_BOUNDARY" &&
            result.diagnostics[3].code == "STRATA.DSL.PARSE_EXPECTED_EXPRESSION" &&
            result.diagnostics[4].code == "STRATA.DSL.PARSE_ARGUMENT_LIST",
        "syntax/security recovery diagnostic ordering changed"
    );
    check(
        result.diagnostics[2].range->start == strata::compiler::SourcePosition{1U, 4U, 3U} &&
            result.diagnostics[2].range->end == strata::compiler::SourcePosition{1U, 22U, 21U},
        "security diagnostic range changed"
    );
    check(result.file.declarations.size() == 1U, "broken declaration did not recover");
}

void test_full_language_ast() {
    const std::string source =
        "component Greeting(label: string) { Text(text: label) }\n"
        "screen Main { state count: number = 1; derived doubled = count * 2; "
        "root Greeting(label: format(\"{0}\", doubled)) }";
    const auto result = strata::compiler::parse_source("app/main.strata", source);
    check(result.diagnostics.empty(), "valid AST fixture produced diagnostics");
    check(result.file.declarations.size() == 2U, "declaration count changed");
    const auto* screen = std::get_if<strata::compiler::ScreenDeclaration>(
        &result.file.declarations[1].node
    );
    check(screen != nullptr && screen->body->statements.size() == 3U, "screen AST shape changed");
    const auto* derived = std::get_if<strata::compiler::DerivedStatement>(
        &screen->body->statements[1]->node
    );
    check(
        derived != nullptr &&
            std::holds_alternative<strata::compiler::BinaryExpression>(derived->expression->node),
        "binary precedence AST changed"
    );
}

void test_compiler_limits_are_structured_and_terminal() {
    {
        strata::compiler::ParserLimits limits;
        limits.max_source_bytes = 4U;
        const auto result = strata::compiler::parse_source(
            "limit.strata",
            "screen Main {}",
            limits
        );
        check(
            result.diagnostics.size() == 1U &&
                has_diagnostic(result, "STRATA.DSL.LIMIT_SOURCE_BYTES"),
            "source byte limit must return one structured diagnostic"
        );
    }
    {
        strata::compiler::ParserLimits limits;
        limits.max_tokens = 3U;
        const auto result = strata::compiler::parse_source(
            "limit.strata",
            "screen Main { root Text(text: \"value\") }",
            limits
        );
        check(
            has_diagnostic(result, "STRATA.DSL.LIMIT_TOKENS"),
            "lexer token limit diagnostic is missing"
        );
    }
    {
        strata::compiler::ParserLimits limits;
        limits.max_nesting_depth = 4U;
        const auto result = strata::compiler::parse_source(
            "limit.strata",
            "screen Main { root Text(text: (((((1))))) ) }",
            limits
        );
        check(
            has_diagnostic(result, "STRATA.DSL.LIMIT_NESTING_DEPTH"),
            "parser nesting limit diagnostic is missing"
        );
    }
    {
        strata::compiler::ParserLimits limits;
        limits.max_declarations = 1U;
        const auto result = strata::compiler::parse_source(
            "limit.strata",
            "screen First {} screen Second {}",
            limits
        );
        check(
            has_diagnostic(result, "STRATA.DSL.LIMIT_DECLARATIONS") &&
                result.file.declarations.size() == 1U,
            "declaration limit must preserve only completed declarations"
        );
    }
    {
        strata::compiler::ParserLimits limits;
        limits.max_statements = 1U;
        const auto result = strata::compiler::parse_source(
            "limit.strata",
            "screen Main { state first = 1; state second = 2; }",
            limits
        );
        check(
            has_diagnostic(result, "STRATA.DSL.LIMIT_STATEMENTS"),
            "statement limit diagnostic is missing"
        );
    }
    {
        strata::compiler::ParserLimits limits;
        limits.max_expressions = 1U;
        const auto result = strata::compiler::parse_source(
            "limit.strata",
            "screen Main { state first = 1; state second = 2; }",
            limits
        );
        check(
            has_diagnostic(result, "STRATA.DSL.LIMIT_EXPRESSIONS"),
            "expression limit diagnostic is missing"
        );
    }
    {
        strata::compiler::ParserLimits limits;
        limits.max_collection_items = 2U;
        const auto result = strata::compiler::parse_source(
            "limit.strata",
            "screen Main { state values = [1, 2, 3]; }",
            limits
        );
        check(
            has_diagnostic(result, "STRATA.DSL.LIMIT_COLLECTION_ITEMS"),
            "per-collection item limit diagnostic is missing"
        );
    }
}

void test_fixed_seed_malformed_programs_terminate_with_valid_spans() {
    constexpr std::array<std::string_view, 30U> fragments{
        "screen", "component", "style", "animation", "Main", "value", "state",
        "root", "if", "else", "for", "in", "when", "Text", "1", "\"text\"",
        "{", "}", "(", ")", "[", "]", ":", ";", ",", ".", "?", "->", "@", "\n",
    };
    std::uint64_t random = 0xD1B54A32D192ED03ULL;
    const auto next = [&random]() {
        random ^= random << 13U;
        random ^= random >> 7U;
        random ^= random << 17U;
        return random;
    };
    for (std::size_t case_index = 0U; case_index < 512U; ++case_index) {
        std::string source;
        const std::size_t fragment_count = 1U + static_cast<std::size_t>(next() % 96U);
        for (std::size_t index = 0U; index < fragment_count; ++index) {
            source.append(fragments[static_cast<std::size_t>(next() % fragments.size())]);
            source.push_back(' ');
        }

        const auto lexed = strata::compiler::Lexer("generated.strata", source).lex();
        for (const auto& token : lexed.tokens) {
            check(token.span.start.offset <= token.span.end.offset, "generated token span is inverted");
            check(token.span.end.offset <= source.size(), "generated token span exceeds its source");
        }

        const auto parsed = strata::compiler::parse_source("generated.strata", source);
        for (const auto& diagnostic : parsed.diagnostics) {
            if (!diagnostic.range.has_value()) continue;
            check(
                diagnostic.range->source_id == "generated.strata",
                "generated diagnostic lost source identity"
            );
            check(
                diagnostic.range->start.offset <= diagnostic.range->end.offset,
                "generated diagnostic range is inverted"
            );
            check(
                diagnostic.range->end.offset <= source.size(),
                "generated diagnostic range exceeds its source"
            );
        }
    }
}

void test_compiled_artifact_binary_round_trip_and_validation() {
    using strata::compiler::CompiledSourceMap;
    using strata::compiler::CompiledSourceMapEntry;
    using strata::compiler::SourcePosition;
    using strata::compiler::SourceSpan;
    using strata::data::JsonValue;

    const JsonValue unit(JsonValue::Object{
        {"kind", JsonValue("unit")},
        {"generation", JsonValue(std::int64_t{7})},
        {"enabled", JsonValue(true)},
        {"weight", JsonValue(1.25)},
        {"children", JsonValue(JsonValue::Array{
            JsonValue(JsonValue::Null{}),
            JsonValue("repeated"),
            JsonValue(JsonValue::Object{{"label", JsonValue("repeated")}}),
        })},
    });
    const CompiledSourceMap source_map{
        "assets/example.strata",
        {
            CompiledSourceMapEntry{
                "screens.Main.root",
                "widget",
                std::optional<std::string>{"Main"},
                SourceSpan{
                    "assets/example.strata",
                    SourcePosition{2U, 3U, 18U},
                    SourcePosition{4U, 9U, 53U},
                    35U,
                    "root Panel {}",
                },
                "Main/root",
            },
        },
    };

    const std::vector<std::uint8_t> encoded =
        strata::compiler::encode_compiled_module_artifact(unit, source_map);
    check(encoded.size() > 16U, "compiled artifact binary header is missing");
    check(
        encoded == strata::compiler::encode_compiled_module_artifact(unit, source_map),
        "compiled artifact encoding must be deterministic"
    );

    const strata::compiler::CompiledModuleArtifact decoded =
        strata::compiler::decode_compiled_module_artifact(encoded);
    check(
        strata::data::materialize_json(decoded.unit.root()) == unit,
        "compiled artifact changed the portable IR"
    );
    check(
        strata::data::encode_canonical_json(decoded.unit.root()) ==
            strata::data::encode_canonical_json(unit),
        "frozen artifact view changed canonical JSON encoding"
    );
    check(
        decoded.source_map.source_id == source_map.source_id &&
            decoded.source_map.entries.size() == 1U,
        "compiled artifact changed the source-map envelope"
    );
    const CompiledSourceMapEntry& entry = decoded.source_map.entries.front();
    check(
        entry.path == source_map.entries.front().path &&
            entry.kind == source_map.entries.front().kind &&
            entry.name == source_map.entries.front().name &&
            entry.span.source_id == source_map.entries.front().span.source_id &&
            entry.span.start == source_map.entries.front().span.start &&
            entry.span.end == source_map.entries.front().span.end &&
            entry.runtime_component_path == source_map.entries.front().runtime_component_path,
        "compiled artifact changed a source-map entry"
    );

    std::vector<std::uint8_t> corrupt = encoded;
    corrupt.front() ^= 0xFFU;
    bool rejected_corrupt = false;
    try {
        static_cast<void>(strata::compiler::decode_compiled_module_artifact(corrupt));
    } catch (const std::runtime_error&) {
        rejected_corrupt = true;
    }
    check(rejected_corrupt, "compiled artifact accepted corrupt magic");

    bool rejected_truncated = false;
    try {
        static_cast<void>(strata::compiler::decode_compiled_module_artifact(
            std::span<const std::uint8_t>(encoded).first(encoded.size() - 1U)
        ));
    } catch (const std::runtime_error&) {
        rejected_truncated = true;
    }
    check(rejected_truncated, "compiled artifact accepted truncated data");

    std::vector<std::uint8_t> invalid_utf8 = encoded;
    constexpr std::string_view repeated = "repeated";
    const auto repeated_at = std::search(
        invalid_utf8.begin(),
        invalid_utf8.end(),
        repeated.begin(),
        repeated.end()
    );
    check(repeated_at != invalid_utf8.end(), "artifact UTF-8 fixture string is missing");
    *repeated_at = 0xFFU;
    bool rejected_invalid_utf8 = false;
    try {
        static_cast<void>(strata::compiler::decode_compiled_module_artifact(invalid_utf8));
    } catch (const std::runtime_error&) {
        rejected_invalid_utf8 = true;
    }
    check(rejected_invalid_utf8, "compiled artifact accepted invalid UTF-8");

    std::vector<std::uint8_t> non_finite = encoded;
    constexpr std::array<std::uint8_t, 8U> finite_bits{
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xF4U, 0x3FU,
    };
    const auto number_at = std::search(
        non_finite.begin(),
        non_finite.end(),
        finite_bits.begin(),
        finite_bits.end()
    );
    check(number_at != non_finite.end(), "artifact finite-number fixture is missing");
    number_at[6] = 0xF0U;
    number_at[7] = 0x7FU;
    bool rejected_non_finite = false;
    try {
        static_cast<void>(strata::compiler::decode_compiled_module_artifact(non_finite));
    } catch (const std::runtime_error&) {
        rejected_non_finite = true;
    }
    check(rejected_non_finite, "compiled artifact accepted a non-finite number");
}

void test_portable_ir_rejects_duplicate_fields_in_narrow_and_wide_objects() {
    using strata::data::JsonValue;
    const auto rejected_duplicate = [](const JsonValue& value) {
        try {
            strata::compiler::validate_portable_ir(value);
        } catch (const std::runtime_error& error) {
            return std::string_view(error.what()).contains("duplicate field");
        }
        return false;
    };

    check(
        rejected_duplicate(JsonValue(JsonValue::Object{
            {"duplicate", JsonValue(JsonValue::Null{})},
            {"duplicate", JsonValue(JsonValue::Null{})},
        })),
        "portable IR narrow-object validation lost duplicate field rejection"
    );

    JsonValue::Object wide;
    for (std::size_t index = 0U; index < 17U; ++index) {
        wide.emplace_back("field" + std::to_string(index), JsonValue(JsonValue::Null{}));
    }
    wide.emplace_back("field0", JsonValue(JsonValue::Null{}));
    check(
        rejected_duplicate(JsonValue(std::move(wide))),
        "portable IR wide-object validation lost duplicate field rejection"
    );
}

} // namespace

int main() {
    try {
        test_utf8_source_positions_use_kotlin_compatible_utf16_offsets();
        test_lexer_contract();
        test_recovery_diagnostics();
        test_parser_and_security_recovery_match_the_corpus_contract();
        test_full_language_ast();
        test_compiler_limits_are_structured_and_terminal();
        test_fixed_seed_malformed_programs_terminate_with_valid_spans();
        test_compiled_artifact_binary_round_trip_and_validation();
        test_portable_ir_rejects_duplicate_fields_in_narrow_and_wide_objects();
        std::cout << "strata_compiler_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_compiler_tests: " << exception.what() << '\n';
        return 1;
    }
}
