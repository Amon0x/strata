#include "interactive.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "data/json.hpp"
#include "session.hpp"

namespace strata::headless {
namespace {

using data::JsonValue;

constexpr std::size_t maximum_request_bytes = 1024U * 1024U;

[[nodiscard]] JsonValue response(JsonValue state, const std::uint64_t sequence, const JsonValue* id,
                                 const bool ok) {
    JsonValue::Object fields = state.object() != nullptr ? *state.object() : JsonValue::Object{};
    fields.emplace_back("ok", JsonValue(ok));
    fields.emplace_back("sequence", JsonValue(static_cast<std::int64_t>(sequence)));
    if (id != nullptr)
        fields.emplace_back("id", *id);
    return JsonValue(std::move(fields));
}

[[nodiscard]] JsonValue error_response(const std::uint64_t sequence, const JsonValue* id,
                                       const std::string_view message) {
    JsonValue::Object fields{
        {"error", JsonValue(std::string(message))},
        {"event", JsonValue("error")},
        {"ok", JsonValue(false)},
        {"protocol", JsonValue("strata.headless.interactive")},
        {"sequence", JsonValue(static_cast<std::int64_t>(sequence))},
        {"version", JsonValue(std::int64_t{1})},
    };
    if (id != nullptr)
        fields.emplace_back("id", *id);
    return JsonValue(std::move(fields));
}

void send(std::ostream& output, const JsonValue& value) {
    output << data::encode_json_line(value) << std::flush;
    if (!output)
        throw std::runtime_error("headless interactive response stream failed");
}

void require_empty_argument(const JsonValue& value, const std::string_view operation) {
    if (value.is_null())
        return;
    if (value.object() == nullptr || !value.object()->empty()) {
        throw std::invalid_argument(std::string(operation) + " must be null or an empty object");
    }
}

} // namespace

void run_interactive(const Scenario& scenario, const std::filesystem::path& resource_root,
                     const std::filesystem::path& output_root, std::istream& input,
                     std::ostream& output) {
    Session session(scenario, resource_root, output_root);
    session.ensure_frame();
    session.write_current();
    send(output, response(session.interactive_state("ready"), 0U, nullptr, true));

    std::uint64_t sequence = 0U;
    std::string line;
    bool explicitly_closed = false;
    while (std::getline(input, line)) {
        if (line.empty())
            continue;
        ++sequence;
        const JsonValue* request_id = nullptr;
        JsonValue request;
        try {
            if (line.size() > maximum_request_bytes) {
                throw std::invalid_argument("interactive request exceeds the 1 MiB limit");
            }
            request = data::parse_json(line);
            const JsonValue::Object* fields = request.object();
            if (fields == nullptr)
                throw std::invalid_argument("interactive request must be an object");
            request_id = request.find("id");
            const JsonValue::ObjectEntry* operation = nullptr;
            for (const JsonValue::ObjectEntry& field : *fields) {
                if (field.first == "id")
                    continue;
                if (operation != nullptr) {
                    throw std::invalid_argument(
                        "interactive request must contain exactly one operation plus optional id");
                }
                operation = &field;
            }
            if (operation == nullptr) {
                throw std::invalid_argument("interactive request contains no operation");
            }

            if (operation->first == "inspect") {
                require_empty_argument(operation->second, "inspect");
                session.ensure_frame();
                session.write_current();
                send(output,
                     response(session.interactive_state("inspect"), sequence, request_id, true));
                continue;
            }
            if (operation->first == "close") {
                require_empty_argument(operation->second, "close");
                session.write_current();
                session.write_result();
                send(output,
                     response(session.interactive_state("closed"), sequence, request_id, true));
                session.close();
                explicitly_closed = true;
                break;
            }

            JsonValue::Object step_fields;
            step_fields.emplace_back(operation->first, operation->second);
            session.execute(parse_scenario_step(JsonValue(std::move(step_fields)), sequence));
            session.write_current();
            send(output,
                 response(session.interactive_state(operation->first), sequence, request_id, true));
        } catch (const std::exception& error) {
            send(output, error_response(sequence, request_id, error.what()));
        }
    }
    if (!explicitly_closed) {
        session.write_result();
        session.close();
    }
}

} // namespace strata::headless
