#include <iostream>
#include <string_view>

int strata_test_abi(int argument_count, const char* const* const arguments);
int strata_test_scale();

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count < 2) {
        std::cerr << "expected test suite name\n";
        return 2;
    }

    const std::string_view suite(arguments[1]);
    if (suite == "abi")
        return strata_test_abi(argument_count - 1, arguments + 1);
    if (suite == "scale")
        return strata_test_scale();

    std::cerr << "unknown test suite: " << suite << '\n';
    return 2;
}
