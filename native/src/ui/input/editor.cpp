#include "ui/input/editor.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/utf8.hpp"

namespace strata::ui {
namespace {

constexpr std::int64_t undo_coalesce_nanos = 600'000'000;
constexpr std::size_t undo_history_limit = 200U;

struct CodePoint final {
    std::uint32_t value = 0U;
    std::size_t start = 0U;
    std::size_t end = 0U;
};

[[nodiscard]] std::vector<CodePoint> decode(const std::string_view text) {
    if (!core::valid_utf8(text)) throw std::invalid_argument("editor text must be valid UTF-8");
    std::vector<CodePoint> result;
    result.reserve(text.size());
    std::size_t index = 0U;
    while (index < text.size()) {
        const std::size_t start = index;
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
        for (std::size_t count = 0U; count < continuation; ++count) {
            value = (value << 6U) | (static_cast<std::uint8_t>(text[index++]) & 0x3FU);
        }
        result.push_back(CodePoint{value, start, index});
    }
    return result;
}

[[nodiscard]] bool in_range(
    const std::uint32_t value,
    const std::uint32_t first,
    const std::uint32_t last
) noexcept {
    return value >= first && value <= last;
}

[[nodiscard]] bool combining(const std::uint32_t value) noexcept {
    return in_range(value, 0x0300U, 0x036FU) || in_range(value, 0x0483U, 0x0489U) ||
           in_range(value, 0x0591U, 0x05BDU) || value == 0x05BFU ||
           in_range(value, 0x05C1U, 0x05C2U) || in_range(value, 0x0610U, 0x061AU) ||
           in_range(value, 0x064BU, 0x065FU) || value == 0x0670U ||
           in_range(value, 0x06D6U, 0x06EDU) || in_range(value, 0x0711U, 0x0711U) ||
           in_range(value, 0x0730U, 0x074AU) || in_range(value, 0x07A6U, 0x07B0U) ||
           in_range(value, 0x07EBU, 0x07F3U) || in_range(value, 0x0816U, 0x082DU) ||
           in_range(value, 0x0859U, 0x085BU) || in_range(value, 0x08D3U, 0x0903U) ||
           in_range(value, 0x093AU, 0x094FU) || in_range(value, 0x0981U, 0x0983U) ||
           in_range(value, 0x09BCU, 0x09CDU) || in_range(value, 0x0A01U, 0x0A4DU) ||
           in_range(value, 0x0A70U, 0x0A71U) || in_range(value, 0x0B01U, 0x0B4DU) ||
           in_range(value, 0x0C00U, 0x0C4DU) || in_range(value, 0x0D00U, 0x0D4DU) ||
           in_range(value, 0x1AB0U, 0x1AFFU) || in_range(value, 0x1DC0U, 0x1DFFU) ||
           in_range(value, 0x20D0U, 0x20FFU) || in_range(value, 0xFE20U, 0xFE2FU) ||
           in_range(value, 0xFE00U, 0xFE0FU) || in_range(value, 0xE0100U, 0xE01EFU) ||
           in_range(value, 0x1F3FBU, 0x1F3FFU);
}

[[nodiscard]] bool regional_indicator(const std::uint32_t value) noexcept {
    return in_range(value, 0x1F1E6U, 0x1F1FFU);
}

[[nodiscard]] std::vector<std::size_t> grapheme_boundaries(const std::string_view text) {
    const std::vector<CodePoint> points = decode(text);
    std::vector<std::size_t> result{0U};
    std::size_t consecutive_regional = 0U;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        if (index != 0U) {
            const std::uint32_t previous = points[index - 1U].value;
            const std::uint32_t current = points[index].value;
            const bool crlf = previous == 0x0DU && current == 0x0AU;
            const bool joined = current == 0x200DU || previous == 0x200DU;
            const bool regional_pair = regional_indicator(previous) &&
                                       regional_indicator(current) &&
                                       consecutive_regional % 2U == 1U;
            if (!crlf && !joined && !combining(current) && !regional_pair) {
                result.push_back(points[index].start);
            }
        }
        consecutive_regional = regional_indicator(points[index].value)
                                   ? consecutive_regional + 1U
                                   : 0U;
    }
    if (result.back() != text.size()) result.push_back(text.size());
    return result;
}

[[nodiscard]] std::size_t floor_boundary(
    const std::string_view text,
    const std::size_t offset
) {
    const std::vector<std::size_t> boundaries = grapheme_boundaries(text);
    const auto found = std::upper_bound(boundaries.begin(), boundaries.end(), std::min(offset, text.size()));
    return found == boundaries.begin() ? 0U : *std::prev(found);
}

[[nodiscard]] std::size_t previous_boundary(
    const std::string_view text,
    const std::size_t offset
) {
    const std::vector<std::size_t> boundaries = grapheme_boundaries(text);
    const auto found = std::lower_bound(boundaries.begin(), boundaries.end(), std::min(offset, text.size()));
    return found == boundaries.begin() ? 0U : *std::prev(found);
}

[[nodiscard]] std::size_t next_boundary(
    const std::string_view text,
    const std::size_t offset
) {
    const std::vector<std::size_t> boundaries = grapheme_boundaries(text);
    const auto found = std::upper_bound(boundaries.begin(), boundaries.end(), std::min(offset, text.size()));
    return found == boundaries.end() ? text.size() : *found;
}

[[nodiscard]] std::uint32_t code_point_at(
    const std::string_view text,
    const std::size_t offset
) {
    const std::vector<CodePoint> points = decode(text);
    const auto found = std::ranges::find(points, floor_boundary(text, offset), &CodePoint::start);
    return found != points.end() ? found->value : 0U;
}

[[nodiscard]] bool whitespace(const std::uint32_t value) noexcept {
    return value == 0x09U || value == 0x0AU || value == 0x0DU || value == 0x20U ||
           value == 0x85U || value == 0xA0U || value == 0x1680U ||
           in_range(value, 0x2000U, 0x200AU) || value == 0x2028U || value == 0x2029U ||
           value == 0x202FU || value == 0x205FU || value == 0x3000U;
}

[[nodiscard]] int word_class(const std::uint32_t value) noexcept {
    if (value == 0x0AU || value == 0x0DU) return 3;
    if (whitespace(value)) return 2;
    if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value == '_' || value >= 0x80U) {
        return 1;
    }
    return 0;
}

[[nodiscard]] std::size_t previous_word(
    const std::string_view text,
    std::size_t offset
) {
    offset = floor_boundary(text, offset);
    while (offset > 0U) {
        const std::size_t previous = previous_boundary(text, offset);
        if (word_class(code_point_at(text, previous)) != 2) break;
        offset = previous;
    }
    if (offset == 0U) return 0U;
    const int kind = word_class(code_point_at(text, previous_boundary(text, offset)));
    while (offset > 0U) {
        const std::size_t previous = previous_boundary(text, offset);
        if (word_class(code_point_at(text, previous)) != kind) break;
        offset = previous;
    }
    return offset;
}

[[nodiscard]] std::size_t next_word(
    const std::string_view text,
    std::size_t offset
) {
    offset = floor_boundary(text, offset);
    if (offset >= text.size()) return text.size();
    const int kind = word_class(code_point_at(text, offset));
    while (offset < text.size() && word_class(code_point_at(text, offset)) == kind) {
        offset = next_boundary(text, offset);
    }
    while (offset < text.size() && word_class(code_point_at(text, offset)) == 2) {
        offset = next_boundary(text, offset);
    }
    return offset;
}

[[nodiscard]] std::size_t code_point_count(const std::string_view text) {
    return decode(text).size();
}

[[nodiscard]] std::size_t line_start(
    const std::string_view text,
    const std::size_t offset
) noexcept {
    const std::size_t clamped = std::min(offset, text.size());
    const std::size_t before = clamped == 0U
                                   ? std::string_view::npos
                                   : text.rfind('\n', clamped - 1U);
    return before == std::string_view::npos ? 0U : before + 1U;
}

[[nodiscard]] std::size_t line_end(
    const std::string_view text,
    const std::size_t offset
) noexcept {
    const std::size_t after = text.find('\n', std::min(offset, text.size()));
    return after == std::string_view::npos ? text.size() : after;
}

[[nodiscard]] std::size_t grapheme_column(
    const std::string_view text,
    const std::size_t start,
    const std::size_t offset
) {
    const std::vector<std::size_t> boundaries = grapheme_boundaries(text);
    return static_cast<std::size_t>(std::ranges::count_if(
        boundaries,
        [start, offset](const std::size_t boundary) {
            return boundary > start && boundary <= offset;
        }
    ));
}

[[nodiscard]] std::size_t offset_at_column(
    const std::string_view text,
    const std::size_t start,
    const std::size_t end,
    const std::size_t column
) {
    const std::vector<std::size_t> boundaries = grapheme_boundaries(text);
    std::size_t result = start;
    std::size_t current = 0U;
    for (const std::size_t boundary : boundaries) {
        if (boundary <= start) continue;
        if (boundary > end || current == column) break;
        result = boundary;
        ++current;
    }
    return std::min(result, end);
}

[[nodiscard]] std::string sanitized(
    const std::string_view input,
    const TextEditorConfig& config
) {
    const std::vector<CodePoint> points = decode(input);
    std::string result;
    result.reserve(input.size());
    for (const CodePoint& point : points) {
        const std::uint32_t value = point.value;
        const bool control = value < 0x20U || in_range(value, 0x7FU, 0x9FU);
        if (control && value != 0x09U && !(config.multiline && value == 0x0AU)) continue;
        const bool ascii_digit = value >= '0' && value <= '9';
        const bool ascii_letter = (value >= 'A' && value <= 'Z') ||
                                  (value >= 'a' && value <= 'z');
        bool accepted = true;
        switch (config.filter) {
        case TextInputFilter::any: break;
        case TextInputFilter::integer:
            accepted = ascii_digit || value == '-' || value == '+';
            break;
        case TextInputFilter::decimal:
            accepted = ascii_digit || value == '-' || value == '+' || value == '.' || value == ',';
            break;
        case TextInputFilter::letters:
            accepted = ascii_letter || value >= 0x80U || whitespace(value) || value == '-' || value == '\'';
            break;
        case TextInputFilter::alphanumeric:
            accepted = ascii_letter || ascii_digit || value >= 0x80U || whitespace(value) ||
                       value == '_' || value == '-';
            break;
        }
        if (accepted) result.append(input.substr(point.start, point.end - point.start));
    }
    return result;
}

[[nodiscard]] std::string truncate_code_points(
    const std::string_view value,
    const std::size_t count
) {
    const std::vector<CodePoint> points = decode(value);
    if (points.size() <= count) return std::string(value);
    return std::string(value.substr(0U, points[count].start));
}

} // namespace

bool TextEditorMutation::changed() const noexcept {
    return text_changed || selection_changed || composition_changed;
}

TextEditorMutation operator+(
    const TextEditorMutation& left,
    const TextEditorMutation& right
) noexcept {
    return TextEditorMutation{
        left.text_changed || right.text_changed,
        left.selection_changed || right.selection_changed,
        left.composition_changed || right.composition_changed,
    };
}

TextEditor::TextEditor(std::string controlled_text)
    : text_(std::move(controlled_text)), controlled_text_(text_), caret_(text_.size()),
      selection_start_(caret_), selection_end_(caret_) {
    if (!core::valid_utf8(text_)) throw std::invalid_argument("controlled editor text must be valid UTF-8");
    clamp_offsets();
}

EditorSnapshot TextEditor::snapshot() const noexcept {
    return EditorSnapshot{
        text_, caret_, selection_start_, selection_end_,
        composition_.has_value() ? std::optional<std::string_view>(*composition_) : std::nullopt,
        composition_selection_start_, composition_selection_end_,
    };
}

const std::string& TextEditor::text() const noexcept { return text_; }

std::optional<std::pair<std::size_t, std::size_t>> TextEditor::selection() const noexcept {
    const std::size_t start = std::min(selection_start_, selection_end_);
    const std::size_t end = std::max(selection_start_, selection_end_);
    return start == end ? std::nullopt : std::optional(std::pair(start, end));
}

std::string TextEditor::selected_text() const {
    const auto range = selection();
    return range.has_value()
               ? text_.substr(range->first, range->second - range->first)
               : std::string{};
}

std::string TextEditor::visual_text() const {
    if (!composition_.has_value() || composition_->empty()) return text_;
    std::string result;
    result.reserve(text_.size() + composition_->size());
    result.append(text_, 0U, caret_);
    result.append(*composition_);
    result.append(text_, caret_, std::string::npos);
    return result;
}

TextEditorMutation TextEditor::reconcile_controlled(const std::string_view source) {
    if (!core::valid_utf8(source)) throw std::invalid_argument("controlled editor text must be valid UTF-8");
    if (source == text_) {
        controlled_text_.assign(source);
        user_edited_ = false;
        clamp_offsets();
        return {};
    }
    if (user_edited_ && source == controlled_text_) {
        clamp_offsets();
        return {};
    }
    const bool composition_changed = composition_.has_value();
    text_.assign(source);
    controlled_text_.assign(source);
    user_edited_ = false;
    clear_composition();
    clamp_offsets();
    return TextEditorMutation{true, true, composition_changed};
}

TextEditorMutation TextEditor::insert(
    const std::string_view value,
    const TextEditorConfig& config,
    const std::int64_t now_nanos
) {
    std::string insertion = sanitized(value, config);
    if (config.max_code_points.has_value()) {
        const auto range = selection();
        const std::size_t selected = range.has_value()
                                       ? code_point_count(text_.substr(
                                             range->first,
                                             range->second - range->first
                                         ))
                                       : 0U;
        const std::size_t retained = code_point_count(text_) - selected;
        const std::size_t available = retained < *config.max_code_points
                                          ? *config.max_code_points - retained
                                          : 0U;
        insertion = truncate_code_points(insertion, available);
    }
    if (insertion.empty()) return {};
    const auto range = selection();
    return replace_range(
        range.has_value() ? range->first : caret_,
        range.has_value() ? range->second : caret_,
        insertion,
        now_nanos
    );
}

TextEditorMutation TextEditor::erase_selection(const std::int64_t now_nanos) {
    const auto range = selection();
    return range.has_value() ? replace_range(range->first, range->second, {}, now_nanos)
                             : TextEditorMutation{};
}

TextEditorMutation TextEditor::backspace(
    const bool by_word,
    const std::int64_t now_nanos
) {
    if (selection().has_value()) return erase_selection(now_nanos);
    const std::size_t previous = by_word ? previous_word(text_, caret_)
                                         : previous_boundary(text_, caret_);
    return previous == caret_ ? TextEditorMutation{}
                              : replace_range(previous, caret_, {}, now_nanos);
}

TextEditorMutation TextEditor::delete_forward(
    const bool by_word,
    const std::int64_t now_nanos
) {
    if (selection().has_value()) return erase_selection(now_nanos);
    const std::size_t next = by_word ? next_word(text_, caret_)
                                     : next_boundary(text_, caret_);
    return next == caret_ ? TextEditorMutation{}
                          : replace_range(caret_, next, {}, now_nanos);
}

TextEditorMutation TextEditor::move_left(
    const bool by_word,
    const bool extend_selection
) {
    if (!extend_selection) {
        if (const auto range = selection(); range.has_value()) return move(range->first, false);
    }
    return move(by_word ? previous_word(text_, caret_) : previous_boundary(text_, caret_), extend_selection);
}

TextEditorMutation TextEditor::move_right(
    const bool by_word,
    const bool extend_selection
) {
    if (!extend_selection) {
        if (const auto range = selection(); range.has_value()) return move(range->second, false);
    }
    return move(by_word ? next_word(text_, caret_) : next_boundary(text_, caret_), extend_selection);
}

TextEditorMutation TextEditor::move_home(const bool extend_selection) {
    return move(0U, extend_selection);
}

TextEditorMutation TextEditor::move_end(const bool extend_selection) {
    return move(text_.size(), extend_selection);
}

TextEditorMutation TextEditor::move_line_home(const bool extend_selection) {
    return move(line_start(text_, caret_), extend_selection);
}

TextEditorMutation TextEditor::move_line_end(const bool extend_selection) {
    return move(line_end(text_, caret_), extend_selection);
}

TextEditorMutation TextEditor::move_up(const bool extend_selection) {
    const std::size_t current_start = line_start(text_, caret_);
    if (current_start == 0U) return move(0U, extend_selection);
    const std::size_t previous_end = current_start - 1U;
    const std::size_t previous_start = line_start(text_, previous_end);
    return move(offset_at_column(
        text_, previous_start, previous_end,
        grapheme_column(text_, current_start, caret_)
    ), extend_selection);
}

TextEditorMutation TextEditor::move_down(const bool extend_selection) {
    const std::size_t current_end = line_end(text_, caret_);
    if (current_end == text_.size()) return move(text_.size(), extend_selection);
    const std::size_t next_start = current_end + 1U;
    const std::size_t next_end = line_end(text_, next_start);
    return move(offset_at_column(
        text_, next_start, next_end,
        grapheme_column(text_, line_start(text_, caret_), caret_)
    ), extend_selection);
}

TextEditorMutation TextEditor::place_caret(
    const std::size_t byte_offset,
    const bool extend_selection
) {
    const bool composition_changed = composition_.has_value();
    clear_composition();
    TextEditorMutation mutation = move(byte_offset, extend_selection);
    mutation.composition_changed = composition_changed;
    return mutation;
}

TextEditorMutation TextEditor::set_selection(
    const std::size_t anchor_byte_offset,
    const std::size_t focus_byte_offset
) {
    const std::size_t anchor = floor_boundary(text_, anchor_byte_offset);
    const std::size_t focus = floor_boundary(text_, focus_byte_offset);
    const bool composition_changed = composition_.has_value();
    const bool changed = caret_ != focus || selection_start_ != anchor ||
                         selection_end_ != focus || composition_changed;
    caret_ = focus;
    selection_start_ = anchor;
    selection_end_ = focus;
    clear_composition();
    return TextEditorMutation{false, changed, composition_changed};
}

TextEditorMutation TextEditor::select_word_at(const std::size_t byte_offset) {
    const bool composition_changed = composition_.has_value();
    clear_composition();
    if (text_.empty()) return TextEditorMutation{false, false, composition_changed};
    const std::size_t anchor = floor_boundary(text_, std::min(byte_offset, text_.size()));
    const std::size_t probe = anchor < text_.size() ? anchor : previous_boundary(text_, anchor);
    const int kind = word_class(code_point_at(text_, probe));
    std::size_t start = probe;
    while (start > 0U) {
        const std::size_t previous = previous_boundary(text_, start);
        if (word_class(code_point_at(text_, previous)) != kind) break;
        start = previous;
    }
    std::size_t end = next_boundary(text_, probe);
    while (end < text_.size() && word_class(code_point_at(text_, end)) == kind) {
        end = next_boundary(text_, end);
    }
    const bool changed = caret_ != end || selection_start_ != start || selection_end_ != end;
    caret_ = end;
    selection_start_ = start;
    selection_end_ = end;
    return TextEditorMutation{false, changed, composition_changed};
}

TextEditorMutation TextEditor::select_line_at(const std::size_t byte_offset) {
    const bool composition_changed = composition_.has_value();
    clear_composition();
    const std::size_t anchor = floor_boundary(text_, std::min(byte_offset, text_.size()));
    const std::size_t start = line_start(text_, anchor);
    const std::size_t end = line_end(text_, anchor);
    const bool changed = caret_ != end || selection_start_ != start || selection_end_ != end;
    caret_ = end;
    selection_start_ = start;
    selection_end_ = end;
    return TextEditorMutation{false, changed, composition_changed};
}

TextEditorMutation TextEditor::select_all() {
    const bool unchanged = caret_ == text_.size() && selection_start_ == 0U &&
                           selection_end_ == text_.size();
    caret_ = text_.size();
    selection_start_ = 0U;
    selection_end_ = text_.size();
    return TextEditorMutation{false, !unchanged, false};
}

TextEditorMutation TextEditor::undo() {
    if (undo_.empty()) return {};
    redo_.push_back(history_snapshot());
    const HistorySnapshot snapshot = std::move(undo_.back());
    undo_.pop_back();
    restore(snapshot);
    return TextEditorMutation{true, true, true};
}

TextEditorMutation TextEditor::redo() {
    if (redo_.empty()) return {};
    undo_.push_back(history_snapshot());
    const HistorySnapshot snapshot = std::move(redo_.back());
    redo_.pop_back();
    restore(snapshot);
    return TextEditorMutation{true, true, true};
}

TextEditorMutation TextEditor::set_preedit(
    std::string value,
    const std::size_t selection_start,
    const std::size_t selection_end,
    const bool multiline
) {
    TextEditorConfig config;
    config.multiline = multiline;
    value = sanitized(value, config);
    std::optional<std::string> next = value.empty()
                                          ? std::nullopt
                                          : std::optional<std::string>(std::move(value));
    const std::size_t start = next.has_value()
                                  ? floor_boundary(*next, std::min(selection_start, next->size()))
                                  : 0U;
    const std::size_t end = next.has_value()
                                ? floor_boundary(*next, std::min(selection_end, next->size()))
                                : 0U;
    const bool changed = composition_ != next || composition_selection_start_ != start ||
                         composition_selection_end_ != end;
    composition_ = std::move(next);
    composition_selection_start_ = start;
    composition_selection_end_ = end;
    return TextEditorMutation{false, false, changed};
}

TextEditorMutation TextEditor::cancel_preedit() {
    const bool changed = composition_.has_value() || composition_selection_start_ != 0U ||
                         composition_selection_end_ != 0U;
    clear_composition();
    return TextEditorMutation{false, false, changed};
}

TextEditorMutation TextEditor::commit_format(const TextCommitFormat format) {
    std::string formatted = text_;
    if (format == TextCommitFormat::trim) {
        const std::vector<CodePoint> points = decode(formatted);
        std::size_t start = 0U;
        std::size_t end = formatted.size();
        for (const CodePoint& point : points) {
            if (!whitespace(point.value)) break;
            start = point.end;
        }
        for (auto point = points.rbegin(); point != points.rend(); ++point) {
            if (!whitespace(point->value)) break;
            end = point->start;
        }
        formatted = start <= end ? formatted.substr(start, end - start) : std::string{};
    } else if (format == TextCommitFormat::uppercase || format == TextCommitFormat::lowercase) {
        std::ranges::transform(formatted, formatted.begin(), [format](const unsigned char value) {
            if (format == TextCommitFormat::uppercase && value >= 'a' && value <= 'z') {
                return static_cast<char>(value - ('a' - 'A'));
            }
            if (format == TextCommitFormat::lowercase && value >= 'A' && value <= 'Z') {
                return static_cast<char>(value + ('a' - 'A'));
            }
            return static_cast<char>(value);
        });
    }
    if (formatted == text_) return cancel_preedit();
    const bool composition_changed = composition_.has_value();
    text_ = std::move(formatted);
    caret_ = text_.size();
    selection_start_ = caret_;
    selection_end_ = caret_;
    user_edited_ = true;
    clear_composition();
    return TextEditorMutation{true, true, composition_changed};
}

TextEditorMutation TextEditor::replace_range(
    const std::size_t start,
    const std::size_t end,
    const std::string_view replacement,
    const std::int64_t now_nanos
) {
    if (!core::valid_utf8(replacement)) throw std::invalid_argument("editor insertion must be valid UTF-8");
    const std::size_t from = floor_boundary(text_, std::min(start, text_.size()));
    const std::size_t to = floor_boundary(text_, std::max(from, std::min(end, text_.size())));
    if (from == to && replacement.empty()) return {};
    push_undo(to - from, replacement.size(), now_nanos);
    redo_.clear();
    const bool composition_changed = composition_.has_value();
    text_.replace(from, to - from, replacement);
    caret_ = from + replacement.size();
    selection_start_ = caret_;
    selection_end_ = caret_;
    user_edited_ = true;
    clear_composition();
    return TextEditorMutation{true, true, composition_changed};
}

TextEditorMutation TextEditor::move(
    const std::size_t requested,
    const bool extend_selection
) {
    const std::size_t next = floor_boundary(text_, std::min(requested, text_.size()));
    if (extend_selection) {
        const std::size_t anchor = selection_start_ == selection_end_
                                       ? caret_
                                       : caret_ == selection_start_ ? selection_end_ : selection_start_;
        const bool unchanged = caret_ == next && selection_start_ == anchor && selection_end_ == next;
        caret_ = next;
        selection_start_ = anchor;
        selection_end_ = next;
        return TextEditorMutation{false, !unchanged, false};
    }
    const bool unchanged = caret_ == next && selection_start_ == next && selection_end_ == next;
    caret_ = next;
    selection_start_ = next;
    selection_end_ = next;
    return TextEditorMutation{false, !unchanged, false};
}

TextEditor::HistorySnapshot TextEditor::history_snapshot() const {
    return HistorySnapshot{text_, caret_, selection_start_, selection_end_};
}

void TextEditor::restore(const HistorySnapshot& snapshot) {
    text_ = snapshot.text;
    caret_ = snapshot.caret;
    selection_start_ = snapshot.selection_start;
    selection_end_ = snapshot.selection_end;
    user_edited_ = true;
    last_undo_push_nanos_ = 0;
    clear_composition();
    clamp_offsets();
}

void TextEditor::push_undo(
    const std::size_t replaced_bytes,
    const std::size_t replacement_bytes,
    const std::int64_t now_nanos
) {
    const bool small = replaced_bytes <= 4U && replacement_bytes <= 4U;
    if (small && !undo_.empty() && last_undo_push_nanos_ != 0 &&
        now_nanos >= last_undo_push_nanos_ &&
        now_nanos - last_undo_push_nanos_ < undo_coalesce_nanos) {
        last_undo_push_nanos_ = now_nanos;
        return;
    }
    undo_.push_back(history_snapshot());
    while (undo_.size() > undo_history_limit) undo_.pop_front();
    last_undo_push_nanos_ = now_nanos;
}

void TextEditor::clear_composition() noexcept {
    composition_.reset();
    composition_selection_start_ = 0U;
    composition_selection_end_ = 0U;
}

void TextEditor::clamp_offsets() {
    caret_ = floor_boundary(text_, std::min(caret_, text_.size()));
    selection_start_ = floor_boundary(text_, std::min(selection_start_, text_.size()));
    selection_end_ = floor_boundary(text_, std::min(selection_end_, text_.size()));
}

} // namespace strata::ui
