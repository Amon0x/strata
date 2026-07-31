#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace strata::compiler {

enum class DiagnosticSeverity {
    info,
    warning,
    error,
};

struct SourcePosition final {
    std::uint32_t line;
    std::uint32_t column;
    std::uint64_t offset;

    [[nodiscard]] friend bool operator==(const SourcePosition&, const SourcePosition&) = default;
};

struct SourceRange final {
    std::string source_id;
    SourcePosition start;
    SourcePosition end;

    [[nodiscard]] friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

struct Diagnostic final {
    std::string code;
    DiagnosticSeverity severity;
    std::string message;
    std::optional<SourceRange> range;
    std::optional<std::string> component_path;
    std::optional<std::string> expected;

    [[nodiscard]] friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

} // namespace strata::compiler
