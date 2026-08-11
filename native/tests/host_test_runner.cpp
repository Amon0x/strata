#include <iostream>
#include <string_view>

int strata_test_host();
int strata_test_browser_model();
int strata_test_showcase_model();

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count != 2) {
        std::cerr << "expected test suite name\n";
        return 2;
    }

    const std::string_view suite(arguments[1]);
    if (suite == "host")
        return strata_test_host();
    if (suite == "browser_model")
        return strata_test_browser_model();
    if (suite == "showcase_model")
        return strata_test_showcase_model();

    std::cerr << "unknown test suite: " << suite << '\n';
    return 2;
}
