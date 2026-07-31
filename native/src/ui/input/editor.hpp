#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace strata::ui {

enum class TextInputFilter { any, integer, decimal, letters, alphanumeric };
enum class TextCommitFormat { none, trim, uppercase, lowercase };

struct TextEditorConfig final {
    bool multiline = false;
    std::optional<std::size_t> max_code_points;
    TextInputFilter filter = TextInputFilter::any;
    TextCommitFormat commit_format = TextCommitFormat::none;
};

struct TextEditorMutation final {
    bool text_changed = false;
    bool selection_changed = false;
    bool composition_changed = false;

    [[nodiscard]] bool changed() const noexcept;
    [[nodiscard]] friend TextEditorMutation operator+(
        const TextEditorMutation& left,
        const TextEditorMutation& right
    ) noexcept;
};

struct EditorSnapshot final {
    std::string_view text;
    std::size_t caret = 0U;
    std::size_t selection_start = 0U;
    std::size_t selection_end = 0U;
    std::optional<std::string_view> composition;
    std::size_t composition_selection_start = 0U;
    std::size_t composition_selection_end = 0U;
};

/**
 * Platform-neutral UTF-8 editor buffer. Offsets are byte offsets and are always normalized to
 * extended-grapheme boundaries; platform adapters never split a UTF-8 sequence or joined emoji.
 * The caller supplies monotonic time for deterministic undo coalescing.
 */
class TextEditor final {
public:
    explicit TextEditor(std::string controlled_text = {});

    [[nodiscard]] EditorSnapshot snapshot() const noexcept;
    [[nodiscard]] const std::string& text() const noexcept;
    [[nodiscard]] std::string selected_text() const;
    [[nodiscard]] std::string visual_text() const;

    [[nodiscard]] TextEditorMutation reconcile_controlled(std::string_view source);
    /** Discards an in-progress edit and adopts a canonical committed value unconditionally. */
    [[nodiscard]] TextEditorMutation restore_controlled(std::string_view source);
    [[nodiscard]] TextEditorMutation insert(
        std::string_view value,
        const TextEditorConfig& config,
        std::int64_t now_nanos
    );
    [[nodiscard]] TextEditorMutation erase_selection(std::int64_t now_nanos);
    [[nodiscard]] TextEditorMutation backspace(bool by_word, std::int64_t now_nanos);
    [[nodiscard]] TextEditorMutation delete_forward(bool by_word, std::int64_t now_nanos);
    [[nodiscard]] TextEditorMutation move_left(bool by_word, bool extend_selection);
    [[nodiscard]] TextEditorMutation move_right(bool by_word, bool extend_selection);
    [[nodiscard]] TextEditorMutation move_home(bool extend_selection);
    [[nodiscard]] TextEditorMutation move_end(bool extend_selection);
    [[nodiscard]] TextEditorMutation move_line_home(bool extend_selection);
    [[nodiscard]] TextEditorMutation move_line_end(bool extend_selection);
    [[nodiscard]] TextEditorMutation move_up(bool extend_selection);
    [[nodiscard]] TextEditorMutation move_down(bool extend_selection);
    [[nodiscard]] TextEditorMutation place_caret(
        std::size_t byte_offset,
        bool extend_selection
    );
    /** Replaces the selection without mutating text; offsets are normalized to grapheme bounds. */
    [[nodiscard]] TextEditorMutation set_selection(
        std::size_t anchor_byte_offset,
        std::size_t focus_byte_offset
    );
    [[nodiscard]] TextEditorMutation select_word_at(std::size_t byte_offset);
    [[nodiscard]] TextEditorMutation select_line_at(std::size_t byte_offset);
    [[nodiscard]] TextEditorMutation select_all();
    [[nodiscard]] TextEditorMutation undo();
    [[nodiscard]] TextEditorMutation redo();
    [[nodiscard]] TextEditorMutation set_preedit(
        std::string value,
        std::size_t selection_start,
        std::size_t selection_end,
        bool multiline
    );
    [[nodiscard]] TextEditorMutation cancel_preedit();
    [[nodiscard]] TextEditorMutation commit_format(TextCommitFormat format);

private:
    struct HistorySnapshot final {
        std::string text;
        std::size_t caret = 0U;
        std::size_t selection_start = 0U;
        std::size_t selection_end = 0U;
    };

    [[nodiscard]] TextEditorMutation replace_range(
        std::size_t start,
        std::size_t end,
        std::string_view replacement,
        std::int64_t now_nanos
    );
    [[nodiscard]] TextEditorMutation move(std::size_t next, bool extend_selection);
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> selection() const noexcept;
    [[nodiscard]] HistorySnapshot history_snapshot() const;
    void restore(const HistorySnapshot& snapshot);
    void push_undo(std::size_t replaced_bytes, std::size_t replacement_bytes, std::int64_t now_nanos);
    void clear_composition() noexcept;
    void clamp_offsets();

    std::string text_;
    std::string controlled_text_;
    std::size_t caret_ = 0U;
    std::size_t selection_start_ = 0U;
    std::size_t selection_end_ = 0U;
    std::optional<std::string> composition_;
    std::size_t composition_selection_start_ = 0U;
    std::size_t composition_selection_end_ = 0U;
    bool user_edited_ = false;
    std::deque<HistorySnapshot> undo_;
    std::deque<HistorySnapshot> redo_;
    std::int64_t last_undo_push_nanos_ = 0;
};

} // namespace strata::ui
