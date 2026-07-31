#include "compiler/source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "core/utf8.hpp"

namespace strata::compiler {
namespace {

[[nodiscard]] std::size_t sequence_length(const unsigned char lead) noexcept {
    if (lead <= 0x7FU) {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3U;
    }
    return 4U;
}

[[nodiscard]] std::uint32_t decode(
    const std::string_view text,
    const std::size_t offset,
    const std::size_t length
) noexcept {
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::uint32_t value = 0U;
    if (length == 1U) {
        return lead;
    }
    if (length == 2U) {
        value = lead & 0x1FU;
    } else if (length == 3U) {
        value = lead & 0x0FU;
    } else {
        value = lead & 0x07U;
    }
    for (std::size_t index = 1U; index < length; ++index) {
        value = (value << 6U) | (static_cast<unsigned char>(text[offset + index]) & 0x3FU);
    }
    return value;
}

} // namespace

SourceRange SourceSpan::range() const {
    return SourceRange{source_id, start, end};
}

SourceBuffer::SourceBuffer(
    std::string source_id,
    std::string text,
    std::pmr::memory_resource* const scratch
)
    : source_id_(std::move(source_id)),
      text_(std::move(text)),
      boundaries_(scratch != nullptr ? scratch : std::pmr::get_default_resource()) {
    if (source_id_.empty()) {
        throw std::invalid_argument("source identity must not be empty");
    }
    if (!strata::core::valid_utf8(source_id_) || !strata::core::valid_utf8(text_)) {
        throw std::invalid_argument("source identity and text must be valid UTF-8");
    }

    std::size_t byte_offset = 0U;
    std::uint64_t utf16_offset = 0U;
    std::uint32_t line = 1U;
    std::uint32_t column = 1U;
    boundaries_.reserve(text_.size() + 1U);
    while (byte_offset < text_.size()) {
        boundaries_.push_back(Boundary{byte_offset, SourcePosition{line, column, utf16_offset}});
        const std::size_t length = sequence_length(static_cast<unsigned char>(text_[byte_offset]));
        const std::uint32_t code_point = decode(text_, byte_offset, length);
        const std::uint32_t utf16_units = code_point > 0xFFFFU ? 2U : 1U;
        utf16_offset += utf16_units;
        byte_offset += length;
        if (code_point == static_cast<std::uint32_t>('\n')) {
            ++line;
            column = 1U;
        } else {
            column += utf16_units;
        }
    }
    boundaries_.push_back(Boundary{byte_offset, SourcePosition{line, column, utf16_offset}});
}

const std::string& SourceBuffer::source_id() const noexcept {
    return source_id_;
}

const std::string& SourceBuffer::text() const noexcept {
    return text_;
}

std::size_t SourceBuffer::size_bytes() const noexcept {
    return text_.size();
}

std::optional<std::uint32_t> SourceBuffer::code_point_at(const std::size_t byte_offset) const noexcept {
    if (byte_offset >= text_.size()) {
        return std::nullopt;
    }
    const std::size_t length = sequence_length(static_cast<unsigned char>(text_[byte_offset]));
    return decode(text_, byte_offset, length);
}

std::size_t SourceBuffer::code_point_bytes_at(const std::size_t byte_offset) const noexcept {
    if (byte_offset >= text_.size()) {
        return 0U;
    }
    return sequence_length(static_cast<unsigned char>(text_[byte_offset]));
}

bool SourceBuffer::starts_with(
    const std::size_t byte_offset,
    const std::string_view value
) const noexcept {
    return byte_offset <= text_.size() && text_.substr(byte_offset).starts_with(value);
}

std::string_view SourceBuffer::slice(const std::size_t start, const std::size_t end) const {
    if (start > end || end > text_.size()) {
        throw std::out_of_range("source slice is outside the source buffer");
    }
    static_cast<void>(boundary(start));
    static_cast<void>(boundary(end));
    return std::string_view(text_).substr(start, end - start);
}

SourceSpan SourceBuffer::span(const std::size_t start, const std::size_t end) const {
    const std::size_t clamped_start = std::min(start, text_.size());
    const std::size_t clamped_end = std::clamp(end, clamped_start, text_.size());
    const Boundary& start_boundary = boundary(clamped_start);
    const Boundary& end_boundary = boundary(clamped_end);
    std::size_t line_end = text_.find('\n', clamped_start);
    if (line_end == std::string::npos) {
        line_end = text_.size();
    }
    std::size_t line_start = clamped_start == 0U
                                 ? std::string::npos
                                 : text_.rfind('\n', clamped_start - 1U);
    line_start = line_start == std::string::npos ? 0U : line_start + 1U;
    if (line_end > line_start && text_[line_end - 1U] == '\r') {
        --line_end;
    }
    return SourceSpan{
        source_id_,
        start_boundary.position,
        end_boundary.position,
        end_boundary.position.offset - start_boundary.position.offset,
        text_.substr(line_start, line_end - line_start),
    };
}

const SourceBuffer::Boundary& SourceBuffer::boundary(const std::size_t byte_offset) const {
    const auto found = std::ranges::lower_bound(boundaries_, byte_offset, {}, &Boundary::byte_offset);
    if (found == boundaries_.end() || found->byte_offset != byte_offset) {
        throw std::out_of_range("source offset is not a UTF-8 code-point boundary");
    }
    return *found;
}

SourceSpan covering(const SourceSpan& left, const SourceSpan& right) {
    if (left.source_id != right.source_id) {
        throw std::invalid_argument("cannot combine spans from different sources");
    }
    const SourceSpan& start = left.start.offset <= right.start.offset ? left : right;
    const SourceSpan& end = left.end.offset >= right.end.offset ? left : right;
    return SourceSpan{
        left.source_id,
        start.start,
        end.end,
        end.end.offset - start.start.offset,
        start.excerpt,
    };
}

} // namespace strata::compiler
