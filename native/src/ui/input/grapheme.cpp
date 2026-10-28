#include "ui/input/grapheme.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <vector>

namespace strata::ui {
namespace {

#include "ui/input/grapheme_break_data.inc"

enum class Break : std::uint8_t {
    other,
    cr,
    lf,
    control,
    extend,
    zwj,
    regional_indicator,
    prepend,
    spacing_mark,
    l,
    v,
    t,
    lv,
    lvt,
};

enum class Conjunct : std::uint8_t { none, linker, consonant, extend };

// GB11: the text before is ExtPict Extend* (pictograph) or ExtPict Extend* ZWJ (joined).
enum class Emoji : std::uint8_t { none, pictograph, joined };
// GB9c: the text before is Consonant [Extend Linker]* (consonant), with a linker among them
// (linked).
enum class Indic : std::uint8_t { none, consonant, linked };

struct Properties final {
    Break kind = Break::other;
    bool pictographic = false;
    Conjunct conjunct = Conjunct::none;
};

[[nodiscard]] Properties properties(const std::uint32_t value) noexcept {
    // Printable ASCII is all Other; it skips the search.
    if (value >= 0x20U && value < 0x7FU)
        return Properties{};
    const auto found = std::upper_bound(grapheme_break_runs.begin(), grapheme_break_runs.end(),
                                        (value << 8U) | 0xFFU);
    const auto packed = found == grapheme_break_runs.begin()
                            ? std::uint8_t{0U}
                            : static_cast<std::uint8_t>(*std::prev(found) & 0xFFU);
    return Properties{
        static_cast<Break>(packed & 0x0FU),
        (packed & 0x10U) != 0U,
        static_cast<Conjunct>((packed >> 5U) & 0x03U),
    };
}

// Decodes the scalar starting at `index` of valid UTF-8 and advances past it.
[[nodiscard]] std::uint32_t decode(const std::string_view text, std::size_t& index) noexcept {
    const auto lead = static_cast<std::uint8_t>(text[index++]);
    std::uint32_t value = lead;
    std::size_t continuation = 0U;
    if ((lead & 0xE0U) == 0xC0U) {
        value = lead & 0x1FU;
        continuation = 1U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        value = lead & 0x0FU;
        continuation = 2U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        value = lead & 0x07U;
        continuation = 3U;
    }
    for (; continuation != 0U && index < text.size(); --continuation)
        value = (value << 6U) | (static_cast<std::uint8_t>(text[index++]) & 0x3FU);
    return value;
}

[[nodiscard]] bool control(const Break kind) noexcept {
    return kind == Break::control || kind == Break::cr || kind == Break::lf;
}

} // namespace

std::string_view grapheme_unicode_version() noexcept {
    return grapheme_data_unicode_version;
}

std::vector<std::size_t> grapheme_boundaries(const std::string_view text) {
    std::vector<std::size_t> result{0U};
    if (text.empty())
        return result;
    std::size_t index = 0U;
    Properties previous = properties(decode(text, index));
    Emoji emoji = previous.pictographic ? Emoji::pictograph : Emoji::none;
    Indic indic = previous.conjunct == Conjunct::consonant ? Indic::consonant : Indic::none;
    // GB12/GB13: regional indicators immediately preceding, pairs break between each other.
    std::size_t regional = previous.kind == Break::regional_indicator ? 1U : 0U;
    while (index < text.size()) {
        const std::size_t start = index;
        const Properties current = properties(decode(text, index));
        const Break before = previous.kind;
        const Break after = current.kind;
        bool join = false;
        if (before == Break::cr && after == Break::lf) {
            join = true; // GB3
        } else if (control(before) || control(after)) {
            join = false; // GB4, GB5
        } else if (before == Break::l && (after == Break::l || after == Break::v ||
                                          after == Break::lv || after == Break::lvt)) {
            join = true; // GB6
        } else if ((before == Break::lv || before == Break::v) &&
                   (after == Break::v || after == Break::t)) {
            join = true; // GB7
        } else if ((before == Break::lvt || before == Break::t) && after == Break::t) {
            join = true; // GB8
        } else if (after == Break::extend || after == Break::zwj || after == Break::spacing_mark ||
                   before == Break::prepend) {
            join = true; // GB9, GB9a, GB9b
        } else if (indic == Indic::linked && current.conjunct == Conjunct::consonant) {
            join = true; // GB9c
        } else if (emoji == Emoji::joined && current.pictographic) {
            join = true; // GB11
        } else if (before == Break::regional_indicator && after == Break::regional_indicator) {
            join = regional % 2U == 1U; // GB12, GB13
        }
        if (!join)
            result.push_back(start);

        if (current.pictographic)
            emoji = Emoji::pictograph;
        else if (emoji == Emoji::pictograph && after == Break::extend)
            emoji = Emoji::pictograph;
        else if (emoji == Emoji::pictograph && after == Break::zwj)
            emoji = Emoji::joined;
        else
            emoji = Emoji::none;

        if (current.conjunct == Conjunct::consonant)
            indic = Indic::consonant;
        else if (indic != Indic::none && current.conjunct == Conjunct::linker)
            indic = Indic::linked;
        else if (indic == Indic::none || current.conjunct != Conjunct::extend)
            indic = Indic::none; // An extend continues the run; anything else ends it.

        regional = after == Break::regional_indicator ? regional + 1U : 0U;
        previous = current;
    }
    result.push_back(text.size());
    return result;
}

} // namespace strata::ui
