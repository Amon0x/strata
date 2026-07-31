#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/diagnostic.hpp"

namespace strata::compiler {

struct SourceSpan final {
    std::string source_id;
    SourcePosition start;
    SourcePosition end;
    std::uint64_t length;
    std::string excerpt;

    [[nodiscard]] SourceRange range() const;
    [[nodiscard]] friend bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

class SourceBuffer final {
public:
    SourceBuffer(
        std::string source_id,
        std::string text,
        std::pmr::memory_resource* scratch = std::pmr::get_default_resource()
    );

    [[nodiscard]] const std::string& source_id() const noexcept;
    [[nodiscard]] const std::string& text() const noexcept;
    [[nodiscard]] std::size_t size_bytes() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> code_point_at(std::size_t byte_offset) const noexcept;
    [[nodiscard]] std::size_t code_point_bytes_at(std::size_t byte_offset) const noexcept;
    [[nodiscard]] bool starts_with(std::size_t byte_offset, std::string_view value) const noexcept;
    [[nodiscard]] std::string_view slice(std::size_t start, std::size_t end) const;
    [[nodiscard]] SourceSpan span(std::size_t start, std::size_t end) const;

private:
    struct Boundary final {
        std::size_t byte_offset;
        SourcePosition position;
    };

    [[nodiscard]] const Boundary& boundary(std::size_t byte_offset) const;

    std::string source_id_;
    std::string text_;
    std::pmr::vector<Boundary> boundaries_;
};

[[nodiscard]] SourceSpan covering(const SourceSpan& left, const SourceSpan& right);

} // namespace strata::compiler
