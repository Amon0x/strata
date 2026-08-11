#include <iostream>
#include <string_view>

int strata_test_svg(int argument_count, const char* const* const arguments);
int strata_test_core();
int strata_test_data();
int strata_test_compiler();
int strata_test_runtime(int argument_count, const char* const* const arguments);
int strata_test_profiler();
int strata_test_resource_host_residual();
int strata_test_ui(int argument_count, const char* const* const arguments);
int strata_test_render_packet(int argument_count, const char* const* const arguments);
int strata_test_text_font_residual(int argument_count, const char* const* arguments);
int strata_test_theme();
int strata_test_input_editor();
int strata_test_chip_input(int argument_count, const char* const* const arguments);
int strata_test_shell_lifecycle(int argument_count, const char* const* const arguments);
int strata_test_interaction_residual(int argument_count, const char* const* const arguments);
int strata_test_widget_binding(int argument_count, const char* const* const arguments);
int strata_test_form_validation(int argument_count, const char* const* const arguments);
int strata_test_command_binding(int argument_count, const char* const* const arguments);

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count < 2) {
        std::cerr << "expected test suite name\n";
        return 2;
    }

    const std::string_view suite(arguments[1]);
    const int suite_argument_count = argument_count - 1;
    const char* const* const suite_arguments = arguments + 1;

    if (suite == "svg")
        return strata_test_svg(suite_argument_count, suite_arguments);
    if (suite == "core")
        return strata_test_core();
    if (suite == "data")
        return strata_test_data();
    if (suite == "compiler")
        return strata_test_compiler();
    if (suite == "runtime")
        return strata_test_runtime(suite_argument_count, suite_arguments);
    if (suite == "profiler")
        return strata_test_profiler();
    if (suite == "resource_host_residual")
        return strata_test_resource_host_residual();
    if (suite == "ui")
        return strata_test_ui(suite_argument_count, suite_arguments);
    if (suite == "render_packet")
        return strata_test_render_packet(suite_argument_count, suite_arguments);
    if (suite == "text_font_residual")
        return strata_test_text_font_residual(suite_argument_count, suite_arguments);
    if (suite == "theme")
        return strata_test_theme();
    if (suite == "input_editor")
        return strata_test_input_editor();
    if (suite == "chip_input")
        return strata_test_chip_input(suite_argument_count, suite_arguments);
    if (suite == "shell_lifecycle")
        return strata_test_shell_lifecycle(suite_argument_count, suite_arguments);
    if (suite == "interaction_residual")
        return strata_test_interaction_residual(suite_argument_count, suite_arguments);
    if (suite == "widget_binding")
        return strata_test_widget_binding(suite_argument_count, suite_arguments);
    if (suite == "form_validation")
        return strata_test_form_validation(suite_argument_count, suite_arguments);
    if (suite == "command_binding")
        return strata_test_command_binding(suite_argument_count, suite_arguments);

    std::cerr << "unknown test suite: " << suite << '\n';
    return 2;
}
