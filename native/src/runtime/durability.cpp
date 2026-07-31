#include "runtime/durability.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"
#include "data/json.hpp"

namespace strata::runtime {
namespace {
using data::JsonValue;

[[nodiscard]] const JsonValue::Object* optional_object(
    const JsonValue& root,
    const std::string_view name
) {
    const JsonValue* value = root.find(name);
    if (value == nullptr) return nullptr;
    if (value->object() == nullptr) {
        throw std::invalid_argument("durable namespace '" + std::string(name) + "' is not an object");
    }
    return value->object();
}

[[nodiscard]] std::optional<std::uint32_t> document_version(const JsonValue& root) {
    const JsonValue* version = root.find("version");
    if (version == nullptr) return 0U; // The development v0 shape predates an explicit version.
    if (const std::int64_t* integer = version->integer();
        integer != nullptr && *integer >= 0 && *integer <= UINT32_MAX) {
        return static_cast<std::uint32_t>(*integer);
    }
    return std::nullopt;
}

void read_values(
    const JsonValue::Object* source,
    std::map<std::string, Value, std::less<>>& target
) {
    if (source == nullptr) return;
    for (const auto& [key, value] : *source) target.insert_or_assign(key, value_from_json(value));
}

[[nodiscard]] JsonValue values_json(
    const std::map<std::string, Value, std::less<>>& values
) {
    JsonValue::Object result;
    result.reserve(values.size());
    for (const auto& [key, value] : values) result.emplace_back(key, value_to_json(value));
    return JsonValue(std::move(result));
}

} // namespace

std::vector<DurableLoadIssue> DurableState::configure(
    std::string application_id,
    DurableStoreAdapter adapter
) {
    validate_key(application_id, "application id");
    application_id_ = std::move(application_id);
    adapter_ = std::move(adapter);
    application_values_.clear();
    widget_values_.clear();
    shell_values_.clear();
    command_recency_.clear();
    generation_ = 0U;
    dirty_ = false;
    configured_ = true;

    std::vector<DurableLoadIssue> issues;
    if (!adapter_.load) return issues;
    std::optional<std::string> payload;
    try {
        payload = adapter_.load(application_id_);
    } catch (const std::exception& error) {
        issues.push_back({
            "STRATA.DURABILITY.LOAD_FAILED",
            "Durable state could not be loaded for '" + application_id_ + "': " + error.what(),
        });
        return issues;
    } catch (...) {
        issues.push_back({
            "STRATA.DURABILITY.LOAD_FAILED",
            "Durable state could not be loaded for '" + application_id_ + "'.",
        });
        return issues;
    }
    if (!payload.has_value() || payload->empty()) return issues;

    try {
        const JsonValue parsed = data::parse_json(*payload);
        if (parsed.object() == nullptr) throw std::invalid_argument("document root is not an object");
        const std::optional<std::uint32_t> version = document_version(parsed);
        if (!version.has_value()) {
            issues.push_back({
                "STRATA.DURABILITY.INVALID_VERSION",
                "Durable state has a non-integer version and was discarded.",
            });
            return issues;
        }
        if (*version > current_version) {
            issues.push_back({
                "STRATA.DURABILITY.UNKNOWN_VERSION",
                "Durable state version " + std::to_string(*version) +
                    " is newer than supported version " + std::to_string(current_version) +
                    " and was discarded.",
            });
            return issues;
        }
        const JsonValue* format = parsed.find("format");
        if (*version != 0U && (format == nullptr || format->string() == nullptr ||
            *format->string() != "strata.durable")) {
            throw std::invalid_argument("durable format marker is missing or unsupported");
        }
        if (format != nullptr && (format->string() == nullptr ||
            *format->string() != "strata.durable")) {
            throw std::invalid_argument("durable format marker is unsupported");
        }
        // v0 and v1 intentionally share namespaces. Reading the old shape is its migration.
        read_values(optional_object(parsed, "application"), application_values_);
        read_values(optional_object(parsed, "shell"), shell_values_);
        if (const JsonValue::Object* widgets = optional_object(parsed, "widgets")) {
            for (const auto& [key, value] : *widgets) {
                if (value.object() == nullptr) {
                    throw std::invalid_argument("durable widget entry '" + key + "' is not an object");
                }
                Values fields;
                read_values(value.object(), fields);
                widget_values_.emplace(key, std::move(fields));
            }
        }
        if (const JsonValue::Object* commands = optional_object(parsed, "commands")) {
            for (const auto& [scope, value] : *commands) {
                const JsonValue::Array* values = value.array();
                if (values == nullptr) {
                    throw std::invalid_argument("durable command scope '" + scope + "' is not an array");
                }
                std::vector<std::string> ids;
                ids.reserve(values->size());
                for (const JsonValue& entry : *values) {
                    if (entry.string() == nullptr || entry.string()->empty()) {
                        throw std::invalid_argument(
                            "durable command scope '" + scope + "' contains an invalid id"
                        );
                    }
                    ids.push_back(*entry.string());
                }
                command_recency_.emplace(scope, std::move(ids));
            }
        }
        ++generation_;
        // A migrated v0 document is rewritten as canonical v1 at the first frame boundary.
        dirty_ = *version == 0U;
    } catch (const data::JsonError& error) {
        issues.push_back({
            "STRATA.DURABILITY.CORRUPT_PAYLOAD",
            "Durable state is corrupt at byte " + std::to_string(error.offset()) +
                " and was discarded.",
        });
        application_values_.clear();
        widget_values_.clear();
        shell_values_.clear();
        command_recency_.clear();
    } catch (const std::exception& error) {
        issues.push_back({
            "STRATA.DURABILITY.MIGRATION_FAILED",
            "Durable state migration failed and the document was discarded: " +
                std::string(error.what()),
        });
        application_values_.clear();
        widget_values_.clear();
        shell_values_.clear();
        command_recency_.clear();
    }
    return issues;
}

bool DurableState::configured() const noexcept { return configured_; }

const Value* DurableState::application_value(const std::string_view key) const noexcept {
    const auto found = application_values_.find(key);
    return found != application_values_.end() ? &found->second : nullptr;
}

void DurableState::set_application_value(std::string key, Value value) {
    validate_key(key, "application persistence key");
    const auto found = application_values_.find(key);
    if (found != application_values_.end() && found->second == value) return;
    application_values_.insert_or_assign(std::move(key), std::move(value));
    changed();
}

bool DurableState::erase_application_value(const std::string_view key) {
    if (application_values_.erase(key) == 0U) return false;
    changed();
    return true;
}

const Value* DurableState::widget_value(
    const std::string_view persistence_key,
    const std::string_view field
) const noexcept {
    const auto widget = widget_values_.find(persistence_key);
    if (widget == widget_values_.end()) return nullptr;
    const auto value = widget->second.find(field);
    return value != widget->second.end() ? &value->second : nullptr;
}

void DurableState::set_widget_value(
    std::string persistence_key,
    std::string field,
    Value value
) {
    validate_key(persistence_key, "widget persistence key");
    validate_key(field, "widget persistence field");
    Values& fields = widget_values_[std::move(persistence_key)];
    const auto found = fields.find(field);
    if (found != fields.end() && found->second == value) return;
    fields.insert_or_assign(std::move(field), std::move(value));
    changed();
}

bool DurableState::erase_widget_value(
    const std::string_view persistence_key,
    const std::string_view field
) {
    const auto widget = widget_values_.find(persistence_key);
    if (widget == widget_values_.end() || widget->second.erase(field) == 0U) return false;
    if (widget->second.empty()) widget_values_.erase(widget);
    changed();
    return true;
}

const Value* DurableState::shell_value(const std::string_view key) const noexcept {
    const auto found = shell_values_.find(key);
    return found != shell_values_.end() ? &found->second : nullptr;
}

void DurableState::set_shell_value(std::string key, Value value) {
    validate_key(key, "shell persistence key");
    const auto found = shell_values_.find(key);
    if (found != shell_values_.end() && found->second == value) return;
    shell_values_.insert_or_assign(std::move(key), std::move(value));
    changed();
}

std::vector<std::string> DurableState::command_recency(const std::string_view scope) const {
    const auto found = command_recency_.find(scope);
    return found != command_recency_.end() ? found->second : std::vector<std::string>{};
}

void DurableState::set_command_recency(
    std::string scope,
    std::vector<std::string> command_ids
) {
    validate_key(scope, "command recency scope");
    command_ids.erase(
        std::ranges::remove_if(command_ids, [](const std::string& id) {
            return id.empty() || !core::valid_utf8(id);
        }).begin(),
        command_ids.end()
    );
    if (command_ids.size() > 64U) command_ids.resize(64U);
    const auto found = command_recency_.find(scope);
    if (found != command_recency_.end() && found->second == command_ids) return;
    command_recency_.insert_or_assign(std::move(scope), std::move(command_ids));
    changed();
}

std::string DurableState::encode() const {
    JsonValue::Object widgets;
    widgets.reserve(widget_values_.size());
    for (const auto& [key, fields] : widget_values_) {
        widgets.emplace_back(key, values_json(fields));
    }
    JsonValue::Object commands;
    commands.reserve(command_recency_.size());
    for (const auto& [scope, ids] : command_recency_) {
        JsonValue::Array values;
        values.reserve(ids.size());
        for (const std::string& id : ids) values.emplace_back(JsonValue(id));
        commands.emplace_back(scope, JsonValue(std::move(values)));
    }
    return data::encode_canonical_json(JsonValue(JsonValue::Object{
        {"application", values_json(application_values_)},
        {"commands", JsonValue(std::move(commands))},
        {"format", JsonValue("strata.durable")},
        {"shell", values_json(shell_values_)},
        {"version", JsonValue(static_cast<std::int64_t>(current_version))},
        {"widgets", JsonValue(std::move(widgets))},
    }));
}

std::optional<DurableLoadIssue> DurableState::flush() {
    if (!configured_ || !dirty_) return std::nullopt;
    if (!adapter_.write) {
        dirty_ = false;
        return std::nullopt;
    }
    try {
        const std::string payload = encode();
        adapter_.write(application_id_, payload);
        dirty_ = false;
        return std::nullopt;
    } catch (const std::exception& error) {
        return DurableLoadIssue{
            "STRATA.DURABILITY.WRITE_FAILED",
            "Durable state write failed for '" + application_id_ + "': " + error.what(),
        };
    } catch (...) {
        return DurableLoadIssue{
            "STRATA.DURABILITY.WRITE_FAILED",
            "Durable state write failed for '" + application_id_ + "'.",
        };
    }
}

bool DurableState::dirty() const noexcept { return dirty_; }
std::uint64_t DurableState::generation() const noexcept { return generation_; }

void DurableState::changed() {
    ++generation_;
    dirty_ = true;
}

void DurableState::validate_key(const std::string_view key, const std::string_view kind) {
    if (key.empty() || !core::valid_utf8(key)) {
        throw std::invalid_argument(std::string(kind) + " must be non-empty valid UTF-8");
    }
}

} // namespace strata::runtime
