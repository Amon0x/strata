#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "font/opentype.hpp"

namespace strata::font {

/** Bounds-checked, lazy decoder for the TrueType loca/glyf outline pair. */
class TrueTypeOutlineSource final {
public:
    TrueTypeOutlineSource(std::span<const std::uint8_t> bytes, std::uint16_t glyph_count);
    ~TrueTypeOutlineSource();

    TrueTypeOutlineSource(const TrueTypeOutlineSource&) = delete;
    TrueTypeOutlineSource& operator=(const TrueTypeOutlineSource&) = delete;
    TrueTypeOutlineSource(TrueTypeOutlineSource&&) noexcept;
    TrueTypeOutlineSource& operator=(TrueTypeOutlineSource&&) noexcept;

    [[nodiscard]] std::shared_ptr<const GlyphOutline> outline(std::uint16_t glyph) const;

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace strata::font
