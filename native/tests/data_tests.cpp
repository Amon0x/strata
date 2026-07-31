#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "data/json.hpp"
#include "resource/resource.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void check_throws(Function&& operation, const std::string_view message) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void test_exact_numbers_and_canonical_json() {
    const auto parsed = strata::data::parse_json(
        "{\"fraction\":1.25e-3,\"minimum\":-9223372036854775808,"
        "\"maximum\":9223372036854775807,\"negativeZero\":-0.0}"
    );
    check(
        *parsed.find("minimum")->integer() == std::numeric_limits<std::int64_t>::min(),
        "minimum int64 lost precision"
    );
    check(
        *parsed.find("maximum")->integer() == std::numeric_limits<std::int64_t>::max(),
        "maximum int64 lost precision"
    );
    check(*parsed.find("negativeZero")->number() == 0.0, "negative zero was not normalized");

    const strata::data::JsonValue value(strata::data::JsonValue::Object{
        {"\xF0\x9F\x98\x80", strata::data::JsonValue(std::int64_t{9})},
        {"a", strata::data::JsonValue(strata::data::JsonValue::Array{
                  strata::data::JsonValue(1.0e20),
                  strata::data::JsonValue(1.0e-6),
                  strata::data::JsonValue("line\ntext"),
              })},
        {"\xEE\x80\x80", strata::data::JsonValue(-0.0)},
    });
    const std::string expected =
        "{\n"
        "  \"a\": [\n"
        "    100000000000000000000,\n"
        "    0.000001,\n"
        "    \"line\\ntext\"\n"
        "  ],\n"
        "  \"\xEE\x80\x80\": 0,\n"
        "  \"\xF0\x9F\x98\x80\": 9\n"
        "}\n";
    const std::string encoded = strata::data::encode_canonical_json(value);
    check(encoded == expected, "canonical JSON ordering/number/escaping changed");
    check(strata::data::parse_json(encoded) == value, "canonical JSON did not round-trip semantically");
    const std::string line = strata::data::encode_json_line(value);
    check(
        line ==
            "{\"a\":[100000000000000000000,0.000001,\"line\\ntext\"],\"\xEE\x80\x80\":0,\"\xF0\x9F\x98\x80\":9}\n",
        "compact JSON line ordering/whitespace changed"
    );
    check(strata::data::parse_json(line) == value, "compact JSON line did not round-trip");
}

void test_strict_json_failures_and_limits() {
    check_throws(
        [] { static_cast<void>(strata::data::parse_json("{\"same\":1,\"same\":2}")); },
        "duplicate object key was accepted"
    );
    check_throws(
        [] { static_cast<void>(strata::data::parse_json("01")); },
        "leading zero was accepted"
    );
    check_throws(
        [] { static_cast<void>(strata::data::parse_json("\"\\uD800\"")); },
        "unpaired surrogate was accepted"
    );
    check_throws(
        [] {
            const strata::data::JsonLimits limits{2U, 8U, 8U, 8U};
            static_cast<void>(strata::data::parse_json("null", limits));
        },
        "input byte limit was ignored"
    );
    check_throws(
        [] {
            const strata::data::JsonLimits limits{128U, 1U, 8U, 8U};
            static_cast<void>(strata::data::parse_json("[[null]]", limits));
        },
        "depth limit was ignored"
    );
}

void test_resource_identity() {
    const auto identity = strata::resource::ResourceId::parse("app/main.strata");
    check(identity.value() == "app/main.strata", "resource identity changed");
    check_throws(
        [] { static_cast<void>(strata::resource::ResourceId::parse("../secret")); },
        "parent traversal was accepted"
    );
    check_throws(
        [] { static_cast<void>(strata::resource::ResourceId::parse("app\\main.strata")); },
        "backslash resource identity was accepted"
    );
    check_throws(
        [] { static_cast<void>(strata::resource::ResourceId::parse("app//main.strata")); },
        "empty resource segment was accepted"
    );
}

} // namespace

int main() {
    try {
        test_exact_numbers_and_canonical_json();
        test_strict_json_failures_and_limits();
        test_resource_identity();
        std::cout << "strata_data_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_data_tests: " << exception.what() << '\n';
        return 1;
    }
}
