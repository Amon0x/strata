#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "font/opentype.hpp"

namespace strata::font {

/** Bounds-checked, lazy decoder for the TrueType loca/glyf outline pair. */
class TrueTypeOutlineSource final {
public:
    TrueTypeOutlineSource(std::span<const std::uint8_t> bytes, std::uint16_t glyph_count);
    /**
     * A variable font's outlines at `coordinates`, normalized per fvar axis (-1 to 1, after avar):
     * each simple glyph moved by its gvar deltas, untouched points interpolated.
     */
    TrueTypeOutlineSource(std::span<const std::uint8_t> bytes, std::uint16_t glyph_count,
                          std::vector<double> coordinates);
    /** How much the coordinates change a glyph's advance (its gvar phantom points), font units. */
    [[nodiscard]] double advance_delta(std::uint16_t glyph) const;
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
