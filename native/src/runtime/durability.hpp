#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"

namespace strata::runtime {

/** Host-owned bytes behind the runtime's typed durable document. */
struct DurableStoreAdapter final {
    /** Missing data is represented by nullopt. Throwing reports a load failure and starts empty. */
    std::function<std::optional<std::string>(std::string_view application_id)> load;
    /** The payload is canonical UTF-8 JSON and remains valid only for the call. */
    std::function<void(std::string_view application_id, std::string_view payload)> write;
};

struct DurableLoadIssue final {
    std::string code;
    std::string message;
};

/**
 * Runtime-owned, explicitly keyed durability document.
 *
 * Retained trees are never serialized. Application values, lifecycle-declared widget fields,
 * shell values, and command recency occupy separate namespaces in one versioned document.
 */
class DurableState final {
public:
    static constexpr std::uint32_t current_version = 1U;

    [[nodiscard]] std::vector<DurableLoadIssue> configure(
        std::string application_id,
        DurableStoreAdapter adapter
    );
    [[nodiscard]] bool configured() const noexcept;

    [[nodiscard]] const Value* application_value(std::string_view key) const noexcept;
    void set_application_value(std::string key, Value value);
    [[nodiscard]] bool erase_application_value(std::string_view key);

    [[nodiscard]] const Value* widget_value(
        std::string_view persistence_key,
        std::string_view field
    ) const noexcept;
    void set_widget_value(std::string persistence_key, std::string field, Value value);
    [[nodiscard]] bool erase_widget_value(
        std::string_view persistence_key,
        std::string_view field
    );

    [[nodiscard]] const Value* shell_value(std::string_view key) const noexcept;
    void set_shell_value(std::string key, Value value);

    [[nodiscard]] std::vector<std::string> command_recency(
        std::string_view scope
    ) const;
    void set_command_recency(std::string scope, std::vector<std::string> command_ids);

    [[nodiscard]] std::string encode() const;
    /** Performs one pending write. Failures keep the document dirty for a later frame. */
    [[nodiscard]] std::optional<DurableLoadIssue> flush();
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    using Values = std::map<std::string, Value, std::less<>>;
    using WidgetValues = std::map<std::string, Values, std::less<>>;

    void changed();
    static void validate_key(std::string_view key, std::string_view kind);

    std::string application_id_;
    DurableStoreAdapter adapter_;
    Values application_values_;
    WidgetValues widget_values_;
    Values shell_values_;
    std::map<std::string, std::vector<std::string>, std::less<>> command_recency_;
    std::uint64_t generation_ = 0U;
    bool configured_ = false;
    bool dirty_ = false;
};

} // namespace strata::runtime
