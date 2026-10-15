// Compiles a contract generated from tests/fixtures/contract_edge_cases.schemas.json: names that
// collide once converted to C++ ("fooBar"/"foo_bar", "café"/"caf", "on"/"ON", two roots naming the
// same function), action arguments called "id" and "decode", an action named "action", a quote in
// an action id, and a namespace containing both "strata" and "std". Building this file is most of
// the test; main checks the renamed members still carry their own wire names.
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "contract_edge_cases.hpp"

namespace {

namespace edge = acme::strata::std;

void check(const bool condition, const ::std::string_view message) {
    if (!condition)
        throw ::std::runtime_error(::std::string(message));
}

} // namespace

int main() {
    try {
        check(edge::wire_name(edge::Mode::on) == "on" && edge::wire_name(edge::Mode::on_2) == "ON",
              "colliding enum values lost their own wire names");
        check(edge::decode_mode(::strata::host::Value("ON")) == edge::Mode::on_2,
              "the second of two colliding enum values did not decode to its own enumerator");

        ::strata::host::Value::Object payload;
        payload.emplace("id", ::strata::host::Value("item-7"));
        ::strata::host::ActionEvent event;
        event.id = "todo.remove";
        event.payload = ::strata::host::Value(::std::move(payload));
        check(edge::TodoRemoveAction::decode(event).id_2 == "item-7",
              "an action argument named 'id' did not decode beside the action's own id");
        check(edge::TodoAbaction::id == "todo.a\"b", "an action id with a quote was not kept");

        ::strata::host::Value::Object app;
        app.emplace("fooBar", ::strata::host::Value("a"));
        app.emplace("foo_bar", ::strata::host::Value("b"));
        app.emplace("caf\xC3\xA9", ::strata::host::Value("c"));
        app.emplace("caf", ::strata::host::Value("d"));
        const edge::App decoded = edge::decode_app(::strata::host::Value(::std::move(app)));
        check(decoded.foo_bar == "a" && decoded.foo_bar_2 == "b" && decoded.caf == "c" &&
                  decoded.caf_2 == "d",
              "colliding object fields did not keep their own values");
        ::std::cout << "strata_contract_edge_cases_tests: OK\n";
        return 0;
    } catch (const ::std::exception& error) {
        ::std::cerr << "strata_contract_edge_cases_tests: " << error.what() << '\n';
        return 1;
    }
}
