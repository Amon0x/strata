#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace strata::ui {

/** Half-open offsets in the original label's canonical UTF-16 coordinate space. */
struct QuickPickMatchSpan final {
    std::size_t start = 0U;
    std::size_t end_exclusive = 0U;

    [[nodiscard]] bool operator==(const QuickPickMatchSpan&) const noexcept = default;
};

/** Borrowed candidate description accepted by the reusable quick-pick matcher. */
struct QuickPickCandidate final {
    std::string_view label;
    std::string_view detail;
    std::size_t source_index = 0U;
    std::int64_t recent_rank = std::numeric_limits<std::int64_t>::min();
};

struct QuickPickMatch final {
    std::size_t candidate_index = 0U;
    std::size_t source_index = 0U;
    std::int64_t recent_rank = std::numeric_limits<std::int64_t>::min();
    int score = 0;
    std::vector<QuickPickMatchSpan> label_spans;
};

/**
 * Locale-independent Unicode quick-pick ranking shared by command palettes and future pickers.
 * Input must be valid UTF-8, as guaranteed by the authoring and public ABI boundaries; malformed
 * bytes still normalize deterministically as one replacement scalar each. Matching operates
 * on Unicode scalars while returned label spans use the original UTF-16 coordinate space.
 */
[[nodiscard]] std::vector<QuickPickMatch> rank_quick_pick(
    std::string_view query,
    std::span<const QuickPickCandidate> candidates
);

} // namespace strata::ui
