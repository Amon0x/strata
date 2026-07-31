#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ui/input/editor.hpp"

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

int main() {
    try {
        test_grapheme_navigation_and_deletion();
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
