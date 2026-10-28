#include <algorithm>
#include <array>
#include <chrono>
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
#include "compiler/compile.hpp"
#include "compiler/lexer.hpp"
#include "compiler/parser.hpp"
#include "compiler/portable_ir.hpp"
#include "compiler/schema.hpp"
#include "compiler/semantic.hpp"
#include "compiler/source.hpp"
#include "resource/resource.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Result>
bool has_diagnostic(const Result& result, const std::string_view code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

template <typename Result>
std::size_t diagnostic_count(const Result& result, const std::string_view code) {
    return static_cast<std::size_t>(std::ranges::count(
        result.diagnostics, code, &strata::compiler::Diagnostic::code
    ));
}

/** Compiles `source` as app/main.strata against the built-in schemas; `modules` serve imports. */
strata::compiler::CompileResult compile_source(
    const std::string_view source,
    const std::vector<strata::compiler::ModuleSource>& modules = {}
) {
    static const strata::compiler::SchemaRegistry registry =
        strata::compiler::SchemaRegistry::builtins();
    const strata::compiler::ModuleLoader loader =
        [&modules](const std::string_view, const std::string_view path) {
            const auto found = std::ranges::find(
                modules, path, &strata::compiler::ModuleSource::source_id
            );
            if (found == modules.end()) {
                throw strata::compiler::ModuleLoadError("test module is missing");
            }
            return *found;
        };
    return strata::compiler::compile_program(
        strata::compiler::ModuleSource{"app/main.strata", std::string(source)},
        loader,
        registry
    );
}

/** Every diagnostic of a compile, one per line, for failure messages. */
std::string describe(const strata::compiler::CompileResult& result) {
    std::string output;
    for (const auto& diagnostic : result.diagnostics) {
        output += "\n  " + diagnostic.code + ": " + diagnostic.message;
    }
    return output;
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

// Syntax errors name what is missing where the author can act on it: the comma between two
// named arguments, and the line of a brace the file never closed.
void test_parser_errors_point_at_the_missing_token() {
    const auto message = [](const std::string_view source) {
        const auto result = strata::compiler::parse_source("syntax.strata", std::string(source));
        return result.diagnostics.empty() ? std::string("<none>")
                                          : result.diagnostics.front().message;
    };
    check(message("overlay Main { root Button(label: \"x\" variant: \"primary\") }") ==
              "Expected ',' before argument 'variant'.",
          "a missing comma between named arguments was not named");
    check(message("overlay Main { root Button(label: \"x\" 3) }") ==
              "Expected ',' or ')' after argument.",
          "a stray token after an argument did not name both continuations");
    check(message("overlay Main {\n  root Panel {\n    Text(text: \"a\")\n") ==
              "Expected '}' to close the block opened at line 2.",
          "an unclosed block was not traced to its opening line");
}

void test_parser_recovery_matches_the_corpus_contract() {
    const std::string source =
        "screen Broken {\n"
        "  state value = 12wat;\n"
        "  root Panel {\n"
        "    Text(text \"missing colon\")\n"
        "    @\n"
        "  }\n"
        "}\n";
    const auto result = strata::compiler::parse_source("app/main.strata", source);
    check(result.diagnostics.size() == 4U, "syntax recovery diagnostic count changed");
    check(
        result.diagnostics[0].code == "STRATA.DSL.LEX_INVALID_NUMBER_SUFFIX" &&
            result.diagnostics[1].code == "STRATA.DSL.LEX_UNEXPECTED_CHAR" &&
            result.diagnostics[2].code == "STRATA.DSL.PARSE_EXPECTED_EXPRESSION" &&
            result.diagnostics[3].code == "STRATA.DSL.PARSE_ARGUMENT_LIST",
        "syntax recovery diagnostic ordering changed"
    );
    check(result.file.declarations.size() == 1U, "broken declaration did not recover");
}

// Source is data the runtime interprets, never host code, so names that happen to match JVM APIs
// are ordinary text wherever they appear.
void test_host_api_names_are_ordinary_text() {
    const auto result = compile_source(
        "/* ProcessBuilder and Class.forName in a comment. */\n"
        "screen Main { root Text(text: \"Runtime.getRuntime and ProcessBuilder\") }"
    );
    check(result.succeeded(), "text naming a JVM API was rejected:" + describe(result));
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
    const strata::compiler::SchemaRegistry registry = strata::compiler::SchemaRegistry::builtins();
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
        // Analysis runs on whatever the parser recovered, recovery nodes included.
        const auto analysed = strata::compiler::validate_semantics(
            parsed.file, registry, strata::compiler::AnalysisInput::incomplete
        );
        std::vector<strata::compiler::Diagnostic> diagnostics = parsed.diagnostics;
        diagnostics.insert(diagnostics.end(), analysed.diagnostics.begin(),
                           analysed.diagnostics.end());
        for (const auto& diagnostic : diagnostics) {
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

// Calls resolve to the first of two same-named components, so the second is reported and never
// validated against the first one's parameters, whether it is declared alongside or imported.
void test_duplicate_components_are_reported_without_reading_the_other_schema() {
    const std::string duplicate =
        "component Card(a: string, b: string = \"x\", c: string = \"x\", d: string = \"x\", "
        "e: string = \"x\", f: string = \"x\", g: string = \"x\", h: string = \"x\") "
        "{ Text(text: a) }\n";
    const auto local = compile_source(
        "component Card(a: string) { Text(text: a) }\n" + duplicate +
        "screen Main { root Card(a: \"x\") }"
    );
    check(diagnostic_count(local, "STRATA.DSL.SEMANTIC_DUPLICATE_DECLARATION") == 1U &&
              local.diagnostics.size() == 1U,
          "a duplicate component was not reported exactly once:" + describe(local));

    const auto imported = compile_source(
        "import \"app/card.strata\";\n"
        "component Card(a: string) { Text(text: a) }\n"
        "screen Main { root Card(a: \"x\") }",
        {strata::compiler::ModuleSource{"app/card.strata", duplicate}}
    );
    check(diagnostic_count(imported, "STRATA.DSL.SEMANTIC_DUPLICATE_DECLARATION") == 1U &&
              imported.diagnostics.size() == 1U,
          "an imported duplicate component was not reported exactly once:" + describe(imported));
}

// Compile time grows with the source, not with the length of its longest line: the same list
// written on one line compiles about as fast as one element per line.
void test_long_single_line_compiles_in_linear_time() {
    constexpr std::size_t elements = 20'000U;
    const auto module = [](const std::string_view separator) {
        std::string source = "screen Main {\n  state values = [";
        for (std::size_t index = 0U; index < elements; ++index) {
            if (index != 0U) source.append(",").append(separator);
            source.append(std::to_string(index));
        }
        source.append("];\n  root Text(text: \"x\")\n}\n");
        return source;
    };
    const auto compile_time = [](const std::string& source) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = compile_source(source);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        check(result.succeeded(), "the long-list fixture did not compile:" + describe(result));
        return elapsed;
    };
    const auto multi_line = compile_time(module("\n"));
    const auto single_line = compile_time(module(" "));
    check(single_line <= multi_line * 3 + std::chrono::milliseconds(100),
          "a long single line compiled " +
              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(single_line)
                                 .count()) +
              "ms against " +
              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(multi_line)
                                 .count()) +
              "ms for the same source on separate lines");
}

// Statement keywords are names wherever a name is expected: after '.', as map keys and arguments.
void test_statement_keywords_are_member_names() {
    const auto result = compile_source(
        "screen Main {\n"
        "  state job = { state: \"s\", from: \"f\", to: \"t\", in: \"i\", where: \"w\", "
        "derived: \"d\", root: \"r\", if: \"c\" };\n"
        "  root Panel {\n"
        "    Text(text: job.state) Text(text: job.from) Text(text: job.to)\n"
        "    Text(text: job.in) Text(text: job.where) Text(text: job.derived)\n"
        "    Text(text: job.root) Text(text: job.if)\n"
        "  }\n"
        "}"
    );
    check(result.succeeded(), "keywords were rejected as member names:" + describe(result));
}

// A when arm written as a bare name matches that name's value; it is not a lambda whose body is
// the arm's block.
void test_when_arm_matches_a_bare_name() {
    const std::string source =
        "screen Main {\n"
        "  state value = \"a\";\n"
        "  state other = \"b\";\n"
        "  root Panel {\n"
        "    when value {\n"
        "      \"a\" -> { Text(text: \"first\") }\n"
        "      other -> { Text(text: \"second\") }\n"
        "      else -> { Text(text: \"third\") }\n"
        "    }\n"
        "  }\n"
        "}";
    const auto parsed = strata::compiler::parse_source("app/main.strata", source);
    check(parsed.diagnostics.empty(), "a bare-name when arm did not parse");
    const auto& screen =
        std::get<strata::compiler::ScreenDeclaration>(parsed.file.declarations.front().node);
    const auto* panel =
        std::get_if<strata::compiler::RootStatement>(&screen.body->statements.back()->node);
    const auto* when = panel != nullptr && panel->call.body != nullptr
        ? std::get_if<strata::compiler::WhenStatement>(&panel->call.body->statements.front()->node)
        : nullptr;
    check(when != nullptr && when->branches.size() == 3U &&
              std::holds_alternative<strata::compiler::IdentifierExpression>(
                  when->branches[1].match->node
              ),
          "a bare-name when arm did not match the name");
    const auto compiled = compile_source(source);
    check(compiled.succeeded(), "a bare-name when arm did not compile:" + describe(compiled));
}

// A map literal is an object with fixed field names, so a computed key is a syntax error rather
// than a field the runtime would see under an invented name.
void test_computed_map_keys_are_rejected() {
    const auto number_key = compile_source(
        "screen Main { state values = { 1: \"a\" }; root Text(text: \"x\") }"
    );
    check(diagnostic_count(number_key, "STRATA.DSL.PARSE_MAP_KEY") == 1U && !number_key.unit,
          "a number map key was accepted:" + describe(number_key));
    const auto member_key = compile_source(
        "screen Main { state item = { id: \"a\" }; state values = { item.id: 1, b: 2 }; "
        "root Text(text: \"x\") }"
    );
    check(diagnostic_count(member_key, "STRATA.DSL.PARSE_MAP_KEY") == 1U && !member_key.unit,
          "a member-access map key was accepted:" + describe(member_key));
}

// Each problem is reported once, however many checks look at the expression behind it.
void test_each_problem_is_reported_once() {
    const auto single = [](const std::string_view source, const std::string_view code) {
        const auto result = compile_source(source);
        check(result.diagnostics.size() == 1U && result.diagnostics.front().code == code,
              std::string(code) + " was not reported exactly once:" + describe(result));
    };
    single("screen Main { root Panel(material: \"nope\") { Text(text: \"a\") } }",
           "STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL");
    single("screen Main { root Panel(material: material(\"nope\")) { Text(text: \"a\") } }",
           "STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL");
    single("screen Main { root Panel(animation: Nope) { Text(text: \"a\") } }",
           "STRATA.DSL.SEMANTIC_UNKNOWN_ANIMATION");
    single("screen Main { root Panel(style: { justifyContent: theme.nope }) { Text(text: \"a\") } }",
           "STRATA.DSL.SEMANTIC_UNKNOWN_THEME_TOKEN");
    single("style Centered { justifyContent: theme.nope }\n"
           "screen Main { root Panel {\n"
           "  Panel(style: Centered) { Text(text: \"a\") }\n"
           "  Panel(style: Centered) { Text(text: \"b\") }\n"
           "} }",
           "STRATA.DSL.SEMANTIC_UNKNOWN_THEME_TOKEN");
}

// A misspelled type is named as such, and lowering reads every declared type exactly as semantic
// analysis does, nullable list elements included.
void test_types_resolve_once_for_analysis_and_lowering() {
    const auto misspelled = compile_source(
        "component Card(label: strnig) { Text(text: label) }\n"
        "screen Main { state names: List<strnig> = []; root Card(label: \"x\") }"
    );
    check(diagnostic_count(misspelled, "STRATA.DSL.SEMANTIC_UNKNOWN_TYPE") == 2U &&
              misspelled.diagnostics.size() == 2U &&
              misspelled.diagnostics.front().message.contains("Did you mean 'string'?"),
          "misspelled types were not reported as unknown types:" + describe(misspelled));

    const auto nullable = compile_source(
        "component Names(names: List<string?>) { Text(text: \"x\") }\n"
        "screen Main { state names: List<string?> = [null]; root Names(names: names) }"
    );
    check(nullable.succeeded(), "nullable list elements did not compile:" + describe(nullable));
    const auto& unit = *nullable.unit;
    const auto& parameter = unit.find("components")->array()->front()
                                .find("parameters")->array()->front();
    const auto* parameter_type = parameter.find("schema")->find("type");
    const auto& screen_body = unit.find("screens")->array()->front().find("body");
    const auto* state_type = screen_body->find("statements")->array()->front()
                                 .find("declaredSchema");
    check(*parameter_type->find("elementNullable")->boolean() &&
              *state_type->find("elementNullable")->boolean(),
          "lowering dropped a nullable list element type");
}

/** The lowered value of argument `name` on the root widget of the first component. */
const strata::data::JsonValue& first_component_argument(
    const strata::compiler::CompileResult& result,
    const std::string_view name
) {
    const auto& component = result.unit->find("components")->array()->front();
    const auto& statement = component.find("body")->find("statements")->array()->front();
    return *statement.find("call")->find("arguments")->find(name);
}

// A name in a style or animation slot is a value when a parameter or local of that name is in
// scope and a declaration otherwise, in semantic analysis and in lowering alike.
void test_slot_names_resolve_the_same_for_analysis_and_lowering() {
    const auto animation = compile_source(
        "animation Fade { from { opacity: 0 } to { opacity: 1 } }\n"
        "component Pulse(a: Animation) { Panel(animation: a) }\n"
        "screen Main { root Pulse(a: Fade) }"
    );
    check(animation.succeeded(), "an animation parameter did not compile:" + describe(animation));
    check(*first_component_argument(animation, "animation").find("kind")->string() == "variable",
          "an animation parameter lowered as a literal animation name");

    const auto style = compile_source(
        "style Card { padding: 4 }\n"
        "component Boxed(Card: Style) { Panel(style: Card) }\n"
        "screen Main { root Boxed(Card: { padding: 8 }) }"
    );
    check(style.succeeded(), "a style parameter did not compile:" + describe(style));
    check(*first_component_argument(style, "style").find("kind")->string() == "variable",
          "a style parameter named like a style lowered as a style reference");

    const auto mistyped = compile_source(
        "style Card { padding: 4 }\n"
        "component Boxed(Card: string) { Panel(style: Card) }\n"
        "screen Main { root Boxed(Card: \"x\") }"
    );
    check(diagnostic_count(mistyped, "STRATA.DSL.SEMANTIC_TYPE_MISMATCH") == 1U,
          "a string parameter shadowing a style was accepted as the style:" + describe(mistyped));
}

// Expressions are typed as the runtime evaluates them: '+' joins text when either side is text,
// arithmetic needs numbers, a conditional is either branch, and a derived value has its type
// wherever it is read, before its declaration included.
void test_expression_types_follow_runtime_evaluation() {
    const auto joined = compile_source(
        "screen Main { state name = \"Ada\"; state count = 2;\n"
        "  root Panel { Text(text: \"Hello \" + name) Text(text: count + \" items\") } }"
    );
    check(joined.succeeded(), "joining text with '+' was rejected:" + describe(joined));

    const auto mismatch = [](const std::string_view source, const std::string_view what) {
        const auto result = compile_source(source);
        check(diagnostic_count(result, "STRATA.DSL.SEMANTIC_TYPE_MISMATCH") == 1U &&
                  result.diagnostics.size() == 1U,
              std::string(what) + " was not reported as one type mismatch:" + describe(result));
    };
    mismatch("screen Main { root Text(text: later); derived later = 5; }",
             "a number derived value read before its declaration");
    mismatch("screen Main { derived first = second; derived second = 5; root Text(text: first) }",
             "a derived value reading a later one");
    mismatch("screen Main { state wide = true; derived label = wide ? \"wide\" : 5;\n"
             "  root Text(text: label) }",
             "a conditional whose else branch is a number");
    mismatch("screen Main { state wide = true; derived twice = wide * 2;\n"
             "  root Text(text: \"x\") }",
             "arithmetic on a boolean");
}

// A list state can hold as many items as lowering declares for it, not only as many as it starts
// with, and a state action reads its target from the arguments after the action id however the
// id is written.
void test_state_actions_see_the_state_lowering_declares() {
    const auto grown = compile_source(
        "screen Main { state items = [1, 2];\n"
        "  root Button(label: \"Add\", onClick: action(\"state.set\", name: \"items\", "
        "value: [1, 2, 3])) }"
    );
    check(grown.succeeded(), "a list state could not grow past its initializer:" + describe(grown));
    const auto& statement = grown.unit->find("screens")->array()->front().find("body")
                                ->find("statements")->array()->front();
    check(*statement.find("declaredSchema")->find("maximumItems")->integer() == 1000,
          "lowering changed the bound it declares for a list state");

    const auto named_id = compile_source(
        "screen Main { state items = [1, 2];\n"
        "  root Button(label: \"Reset\", onClick: action(name: \"state.set\", name: \"items\", "
        "value: [4])) }"
    );
    check(named_id.succeeded(),
          "a named action id was read as the state action's target:" + describe(named_id));
}

// Persisted values are stored by key, so two widgets written side by side cannot share one; a
// component drawn twice is one widget and keeps its key.
void test_sibling_widgets_need_their_own_persistence_keys() {
    const auto siblings = compile_source(
        "screen Main { root Panel {\n"
        "  Scroll(persistenceKey: \"log\") { Text(text: \"a\") }\n"
        "  Scroll(persistenceKey: \"log\") { Text(text: \"b\") }\n"
        "} }"
    );
    check(diagnostic_count(siblings, "STRATA.DSL.SEMANTIC_DUPLICATE_PERSISTENCE_KEY") == 1U,
          "sibling widgets shared a persistence key:" + describe(siblings));

    const auto reused = compile_source(
        "component Log { Scroll(persistenceKey: \"log\") { Text(text: \"a\") } }\n"
        "screen Main { root Panel { Log Log } }"
    );
    check(reused.succeeded(), "a component drawn twice lost its persistence key:" + describe(reused));
}

// A component can forward a slot by declaring it inside what it passes to a child component,
// whether in a named fill or in raw content.
void test_slots_declared_inside_child_component_fills_are_forwarded() {
    const auto forwarded = compile_source(
        "component Frame { Panel { Slot(name: \"content\") } }\n"
        "component Named { Frame { Slot(name: \"content\") { Slot(name: \"inner\") } } }\n"
        "component Raw { Frame { Panel { Slot(name: \"body\") } } }\n"
        "screen Main { root Panel {\n"
        "  Named { Slot(name: \"inner\") { Text(text: \"a\") } }\n"
        "  Raw { Slot(name: \"body\") { Text(text: \"b\") } }\n"
        "} }"
    );
    check(forwarded.succeeded(), "a forwarded slot was not declared:" + describe(forwarded));
}

namespace compiler = strata::compiler;

/** Every node id of a file: expressions, statements, blocks and widget calls. */
class NodeIds final {
public:
    void file(const compiler::File& file) {
        for (const compiler::Declaration& declaration : file.declarations) {
            if (const auto* screen = std::get_if<compiler::ScreenDeclaration>(&declaration.node))
                block(*screen->body);
            else if (const auto* component =
                         std::get_if<compiler::ComponentDeclaration>(&declaration.node)) {
                for (const auto& parameter : component->parameters) {
                    if (parameter.default_value != nullptr) expression(*parameter.default_value);
                }
                block(*component->body);
            }
        }
    }

    std::vector<compiler::NodeId> ids;
    std::vector<const compiler::Expression*> identifiers;

private:
    void block(const compiler::Block& block) {
        ids.push_back(block.id);
        for (const compiler::StatementPtr& statement : block.statements) {
            ids.push_back(statement->id);
            if (const auto* state = std::get_if<compiler::StateStatement>(&statement->node)) {
                if (state->initializer != nullptr) expression(*state->initializer);
            } else if (const auto* derived =
                           std::get_if<compiler::DerivedStatement>(&statement->node)) {
                expression(*derived->expression);
            } else if (const auto* widget =
                           std::get_if<compiler::WidgetStatement>(&statement->node)) {
                call(widget->call);
            } else if (const auto* root = std::get_if<compiler::RootStatement>(&statement->node)) {
                call(root->call);
            } else if (const auto* loop = std::get_if<compiler::ForStatement>(&statement->node)) {
                expression(*loop->collection);
                this->block(*loop->block);
            }
        }
    }

    void call(const compiler::WidgetCall& call) {
        ids.push_back(call.id);
        for (const compiler::Argument& argument : call.arguments) expression(*argument.value);
        if (call.body != nullptr) block(*call.body);
    }

    void expression(const compiler::Expression& expression) {
        ids.push_back(expression.id);
        if (std::holds_alternative<compiler::IdentifierExpression>(expression.node))
            identifiers.push_back(&expression);
        if (const auto* binary = std::get_if<compiler::BinaryExpression>(&expression.node)) {
            this->expression(*binary->left);
            this->expression(*binary->right);
        } else if (const auto* call = std::get_if<compiler::CallExpression>(&expression.node)) {
            for (const compiler::Argument& argument : call->arguments)
                this->expression(*argument.value);
        }
    }
};

/** Loads `main` with `modules` serving its imports, without analysing it. */
compiler::ModuleResult load(const std::string_view main,
                            const std::vector<compiler::ModuleSource>& modules = {}) {
    const compiler::ModuleLoader loader = [&modules](const std::string_view,
                                                     const std::string_view path) {
        const auto found = std::ranges::find(modules, path, &compiler::ModuleSource::source_id);
        if (found == modules.end()) throw compiler::ModuleLoadError("test module is missing");
        return *found;
    };
    return compiler::load_modules(compiler::ModuleSource{"app/main.strata", std::string(main)},
                                  loader);
}

// Every node of a compilation has its own id, across imported modules and through the parser's
// recovery nodes, so analysis can key its tables by id.
void test_node_ids_are_unique_across_modules() {
    const auto modules = load(
        "import \"shared.strata\";\n"
        "screen Main { state count = 1; root Panel { Label(text: \"a\") Text(text: ) } }",
        {compiler::ModuleSource{
            "shared.strata", "component Label(text: string) { Text(text: text + \"!\") }"}}
    );
    NodeIds walk;
    walk.file(modules.merged_file);
    std::vector<compiler::NodeId> ids = walk.ids;
    std::ranges::sort(ids);
    check(std::ranges::adjacent_find(ids) == ids.end(), "two nodes share an id");
    check(!ids.empty() && ids.back() < modules.merged_file.node_id_limit,
          "a node id lies outside the file's id range");
}

/** The identifier named `name` that `walk` found, in source order. */
const compiler::Expression& identifier(const NodeIds& walk, const std::string_view name) {
    for (const compiler::Expression* expression : walk.identifiers) {
        if (std::get<compiler::IdentifierExpression>(expression->node).name == name)
            return *expression;
    }
    throw std::runtime_error("identifier '" + std::string(name) + "' was not found");
}

// Analysis records what each name denotes, each expression's type and each call's parameters,
// and remembers which module a declaration came from; lowering only reads these tables.
void test_analysis_records_a_resolved_typed_tree() {
    static const compiler::SchemaRegistry registry = compiler::SchemaRegistry::builtins();
    const auto modules = load(
        "import \"shared.strata\";\n"
        "screen Main { state count = 1; derived doubled = count * 2;\n"
        "  root Label(text: format(\"{0}\", doubled)) }",
        {compiler::ModuleSource{"shared.strata",
                                "component Label(text: string) { Text(text: text) }"}}
    );
    const compiler::SemanticResult analysed =
        compiler::validate_semantics(modules.merged_file, registry);
    check(analysed.diagnostics.empty(), "the resolved-tree fixture did not analyse cleanly");
    const compiler::SemanticModel& model = analysed.model;
    NodeIds walk;
    walk.file(modules.merged_file);

    const auto kind_of = [&](const std::string_view name) {
        return model.symbol(identifier(walk, name))->kind;
    };
    check(kind_of("text") == compiler::SymbolKind::parameter &&
              kind_of("count") == compiler::SymbolKind::state &&
              kind_of("doubled") == compiler::SymbolKind::derived,
          "identifiers did not resolve to their declarations");
    const compiler::SemanticTypePtr& doubled_type = model.facts(identifier(walk, "doubled")).type;
    check(doubled_type != nullptr && doubled_type->kind == compiler::SemanticTypeKind::number,
          "a derived value was not typed by its expression");

    const auto& screen = std::get<compiler::ScreenDeclaration>(
        modules.merged_file.declarations.back().node
    );
    const auto& root = std::get<compiler::RootStatement>(screen.body->statements.back()->node);
    const compiler::ResolvedWidgetCall* call = model.widget_call(root.call);
    check(call != nullptr && call->target == compiler::WidgetCallTarget::component &&
              call->arguments.size() == 1U && call->arguments.front().parameter != nullptr &&
              call->arguments.front().parameter->name == "text",
          "a component call did not record which parameter its argument fills");
    check(model.components.at("Label").source_id == "shared.strata",
          "an imported component lost the module it was declared in");
}

// `bind:` expands only for a retained state or Binding parameter. A loop item that shadows a
// state is neither: lowering used to drop the binding silently, so it is now an error.
void test_bind_shorthand_needs_a_retained_target() {
    static const compiler::SchemaRegistry registry = compiler::SchemaRegistry::builtins();
    const auto bound = load("screen Main { state name = \"\"; root TextBox(bind: name) }");
    const compiler::SemanticResult analysed =
        compiler::validate_semantics(bound.merged_file, registry);
    const auto& screen =
        std::get<compiler::ScreenDeclaration>(bound.merged_file.declarations.front().node);
    const auto& root = std::get<compiler::RootStatement>(screen.body->statements.back()->node);
    check(analysed.diagnostics.empty() && analysed.model.widget_call(root.call)->bindings.size() == 1U,
          "a state bound with bind: was not recorded for expansion");

    const auto shadowed = compile_source(
        "screen Main { state name = \"\"; state names = [\"a\"];\n"
        "  root Panel { for name in names { TextBox(bind: name) } } }"
    );
    check(diagnostic_count(shadowed, "STRATA.DSL.SEMANTIC_BINDING_TARGET") == 1U,
          "bind: on a loop item shadowing a state was accepted:" + describe(shadowed));
}

// Analysis runs when parsing or an import failed, so every problem shows at once, without the
// cascade a missing declaration or unreadable statement would cause.
void test_analysis_runs_despite_syntax_and_import_errors() {
    const auto broken = compile_source(
        "screen Main { root Panel { Text(text: 5) Text(text \"x\") } }"
    );
    check(has_diagnostic(broken, "STRATA.DSL.PARSE_ARGUMENT_LIST") &&
              diagnostic_count(broken, "STRATA.DSL.SEMANTIC_TYPE_MISMATCH") == 1U &&
              !has_diagnostic(broken, "STRATA.DSL.SEMANTIC_LAYER_ROOT"),
          "a syntax error hid or cascaded into semantic diagnostics:" + describe(broken));
    check(!broken.source_map.entries.empty(), "a module with a syntax error had no source map");

    const auto missing = compile_source("import \"missing.strata\";\nscreen Main { root Missing() }");
    check(has_diagnostic(missing, "STRATA.DSL.IMPORT_LOAD_FAILED") &&
              !has_diagnostic(missing, "STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET"),
          "a failed import cascaded into unknown names:" + describe(missing));
}

} // namespace

int strata_test_compiler() {
    try {
        test_utf8_source_positions_use_kotlin_compatible_utf16_offsets();
        test_lexer_contract();
        test_recovery_diagnostics();
        test_parser_errors_point_at_the_missing_token();
        test_parser_recovery_matches_the_corpus_contract();
        test_host_api_names_are_ordinary_text();
        test_full_language_ast();
        test_compiler_limits_are_structured_and_terminal();
        test_fixed_seed_malformed_programs_terminate_with_valid_spans();
        test_compiled_artifact_binary_round_trip_and_validation();
        test_portable_ir_rejects_duplicate_fields_in_narrow_and_wide_objects();
        test_duplicate_components_are_reported_without_reading_the_other_schema();
        test_long_single_line_compiles_in_linear_time();
        test_statement_keywords_are_member_names();
        test_when_arm_matches_a_bare_name();
        test_computed_map_keys_are_rejected();
        test_each_problem_is_reported_once();
        test_types_resolve_once_for_analysis_and_lowering();
        test_slot_names_resolve_the_same_for_analysis_and_lowering();
        test_expression_types_follow_runtime_evaluation();
        test_state_actions_see_the_state_lowering_declares();
        test_sibling_widgets_need_their_own_persistence_keys();
        test_slots_declared_inside_child_component_fills_are_forwarded();
        test_node_ids_are_unique_across_modules();
        test_analysis_records_a_resolved_typed_tree();
        test_bind_shorthand_needs_a_retained_target();
        test_analysis_runs_despite_syntax_and_import_errors();
        std::cout << "strata_compiler_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_compiler_tests: " << exception.what() << '\n';
        return 1;
    }
}
