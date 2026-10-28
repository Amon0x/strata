#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ui/input/editor.hpp"
#include "ui/input/grapheme.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void test_grapheme_navigation_and_deletion() {
    using namespace strata::ui;
    TextEditor editor("A\x65\xCC\x81\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBBZ");
    static_cast<void>(editor.move_left(false, false));
    check(editor.snapshot().caret == editor.text().size() - 1U, "left did not move over ASCII grapheme");
    static_cast<void>(editor.move_left(false, false));
    check(editor.snapshot().caret == 4U, "left split a joined emoji grapheme");
    static_cast<void>(editor.backspace(false, 1'000));
    check(editor.text() == "A\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBBZ", "backspace split a combining sequence");
}

void test_extended_grapheme_clusters() {
    using namespace strata::ui;
    struct Case final {
        std::string_view name;
        std::string_view text;
        std::vector<std::size_t> boundaries;
    };
    // Expected boundaries are Intl.Segmenter's (ICU, Unicode 17.0): Indic conjuncts and vowel
    // signs, Gurmukhi consonants that stay apart, Thai/Khmer/Myanmar/Sinhala marks, Hangul jamo
    // syllables, emoji tag and ZWJ sequences, flags, prepended marks, and CR LF.
    const std::vector<Case> cases{
        {"devanagari", "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87", {0, 12, 15, 18, 30}},
        {"gurmukhi", "\xE0\xA8\x95\xE0\xA8\x96\xE0\xA8\xBF", {0, 3, 9}},
        {"gujarati", "\xE0\xAA\x95\xE0\xAA\xBF\xE0\xAA\x95\xE0\xAB\x8D\xE0\xAA\xB7", {0, 6, 15}},
        {"thai", "\xE0\xB8\x81\xE0\xB8\xB3\xE0\xB8\x97\xE0\xB8\xB5\xE0\xB9\x88", {0, 6, 15}},
        {"kannada", "\xE0\xB2\x95\xE0\xB2\xBF\xE0\xB2\x95", {0, 6, 9}},
        {"tamil", "\xE0\xAE\x95\xE0\xAF\x81\xE0\xAE\x95\xE0\xAF\x8D\xE0\xAE\xB7", {0, 6, 12, 15}},
        {"sinhala", "\xE0\xB6\x9A\xE0\xB7\x8F\xE0\xB6\x9A", {0, 6, 9}},
        {"khmer", "\xE1\x9E\x80\xE1\x9E\xB6\xE1\x9E\x80\xE1\x9F\x92\xE1\x9E\x80", {0, 6, 15}},
        {"myanmar", "\xE1\x80\x80\xE1\x80\xB1\xE1\x80\x80\xE1\x80\xBA", {0, 6, 12}},
        {"hangul", "\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8\xE1\x84\x80\xE1\x84\x80\xE1\x85\xA1\xEA\xB0\x80\xE1\x86\xA8\xE1\x85\xA1\xE1\x84\x80", {0, 9, 18, 24, 27, 30}},
        {"emoji", "\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7\xF0\x9F\x87\xBA\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", {0, 28, 36, 44, 48, 59, 67}},
        {"prepend", "\xD8\x80\xD9\xA1\xD9\xA1", {0, 4, 6}},
        {"controls", "\x61\x0D\x0A\x62\xCC\x88\x0D\xCC\x88", {0, 1, 3, 6, 7, 9}},
    };
    check(grapheme_unicode_version() == "17.0",
          "grapheme data is not the documented Unicode version");
    for (const Case& value : cases) {
        if (grapheme_boundaries(value.text) != value.boundaries) {
            throw std::runtime_error("UAX #29 grapheme clusters differ for " +
                                     std::string(value.name));
        }
    }
    check(grapheme_boundaries("") == std::vector<std::size_t>{0U}, "empty text has one boundary");

    // A caret never stops inside a Devanagari conjunct.
    TextEditor editor("\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA4\xBF");
    static_cast<void>(editor.move_left(false, false));
    check(editor.snapshot().caret == 0U, "left stopped inside an Indic conjunct");
    static_cast<void>(editor.place_caret(6U, false));
    check(editor.snapshot().caret == 0U, "a caret was placed inside an Indic conjunct");
}

void test_long_text_navigation_is_not_quadratic() {
    using namespace strata::ui;
    // One 20,000-grapheme word: word movement walks every boundary in it, and every boundary
    // lookup used to segment the whole text again.
    std::string word;
    for (int index = 0; index < 20'000; ++index) word += "e\xCC\x81";
    TextEditor editor(word);
    const auto started = std::chrono::steady_clock::now();
    static_cast<void>(editor.move_left(true, false));
    check(editor.snapshot().caret == 0U, "word-left did not cross the long word");
    static_cast<void>(editor.move_right(true, false));
    for (int step = 0; step < 2'000; ++step) {
        static_cast<void>(editor.move_left(false, false));
        static_cast<void>(editor.reconcile_controlled(word));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(editor.snapshot().caret == word.size() - 6'000U, "grapheme movement lost its place");
    check(elapsed < std::chrono::seconds(2),
          "editor navigation re-segments its text per operation");
}

void test_word_selection_undo_and_redo() {
    using namespace strata::ui;
    TextEditor editor("alpha beta");
    static_cast<void>(editor.move_left(true, false));
    check(editor.snapshot().caret == 6U, "word-left did not reach the previous word boundary");
    static_cast<void>(editor.move_left(true, true));
    check(editor.selected_text() == "alpha ", "extended word movement lost its selection anchor");
    static_cast<void>(editor.insert("gamma", TextEditorConfig{}, 1'000));
    check(editor.text() == "gammabeta", "selection replacement failed");
    static_cast<void>(editor.undo());
    check(editor.text() == "alpha beta", "undo did not restore text and selection");
    static_cast<void>(editor.redo());
    check(editor.text() == "gammabeta", "redo did not restore the edit");
}

void test_controlled_reconciliation_and_composition() {
    using namespace strata::ui;
    TextEditor editor("seed");
    static_cast<void>(editor.select_all());
    static_cast<void>(editor.insert("local", TextEditorConfig{}, 1'000));
    static_cast<void>(editor.reconcile_controlled("seed"));
    check(editor.text() == "local", "stale controlled value replaced an optimistic local edit");
    static_cast<void>(editor.reconcile_controlled("local"));
    static_cast<void>(editor.reconcile_controlled("host"));
    check(editor.text() == "host", "new authoritative controlled value was not adopted");
    static_cast<void>(editor.set_preedit("ime", 1U, 2U, false));
    check(editor.visual_text() == "hostime", "preedit was not projected at the caret");
    static_cast<void>(editor.cancel_preedit());
    check(!editor.snapshot().composition.has_value(), "composition cancel left stale preedit state");
}

void test_filter_length_and_commit_format() {
    using namespace strata::ui;
    TextEditor editor;
    TextEditorConfig config;
    config.filter = TextInputFilter::decimal;
    config.max_code_points = 5U;
    static_cast<void>(editor.insert("1a2.34x", config, 1'000));
    check(editor.text() == "12.34", "filter or code-point limit accepted invalid input");

    TextEditor formatted("  Mixed Case  ");
    static_cast<void>(formatted.commit_format(TextCommitFormat::trim));
    check(formatted.text() == "Mixed Case", "trim commit format failed");
    static_cast<void>(formatted.commit_format(TextCommitFormat::uppercase));
    check(formatted.text() == "MIXED CASE", "uppercase commit format failed");
}

void test_multiline_navigation() {
    using namespace strata::ui;
    TextEditor editor("abc\nde\nfghi");
    static_cast<void>(editor.move_up(false));
    check(editor.snapshot().caret == 6U, "up did not clamp to the previous hard line");
    static_cast<void>(editor.move_up(false));
    check(editor.snapshot().caret == 2U, "up did not preserve the grapheme column");
    static_cast<void>(editor.move_down(false));
    check(editor.snapshot().caret == 6U, "down did not preserve the grapheme column");
    static_cast<void>(editor.move_line_home(false));
    check(editor.snapshot().caret == 4U, "line-home moved to the document boundary");
    static_cast<void>(editor.move_line_end(false));
    check(editor.snapshot().caret == 6U, "line-end moved to the document boundary");
}

void test_pointer_selection_primitives() {
    using namespace strata::ui;
    TextEditor editor("alpha beta\ngamma");
    static_cast<void>(editor.place_caret(2U, false));
    check(editor.snapshot().caret == 2U, "pointer caret placement used the wrong byte offset");
    static_cast<void>(editor.place_caret(7U, true));
    check(editor.selected_text() == "pha b", "shift pointer placement lost its anchor");
    static_cast<void>(editor.select_word_at(8U));
    check(editor.selected_text() == "beta", "double-click word selection chose the wrong class run");
    static_cast<void>(editor.select_line_at(13U));
    check(editor.selected_text() == "gamma", "triple-click line selection crossed a hard line");
}

} // namespace

int strata_test_input_editor() {
    try {
        test_grapheme_navigation_and_deletion();
        test_extended_grapheme_clusters();
        test_long_text_navigation_is_not_quadratic();
        test_word_selection_undo_and_redo();
        test_controlled_reconciliation_and_composition();
        test_filter_length_and_commit_format();
        test_multiline_navigation();
        test_pointer_selection_primitives();
        std::cout << "strata_input_editor_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_input_editor_tests: " << error.what() << '\n';
        return 1;
    }
}
