#include "compiler/artifact.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/utf8.hpp"

namespace strata::compiler {
namespace {

using data::JsonValue;
using Bytes = std::vector<std::uint8_t>;

constexpr std::array<std::uint8_t, 8U> magic{
    'S', 'T', 'R', 'A', 'T', 'A', 'C', '\0',
};

enum class ValueTag : std::uint8_t {
    null_value,
    false_value,
    true_value,
    integer,
    number,
    string,
    array,
    object,
};

template <typename Integer>
    requires std::is_unsigned_v<Integer>
void write_integer(Bytes& output, const Integer value) {
    for (std::size_t byte = 0U; byte < sizeof(Integer); ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

[[nodiscard]] std::uint32_t checked_count(
    const std::size_t value,
    const std::string_view label
) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string(label) + " exceeds the compiled artifact limit");
    }
    return static_cast<std::uint32_t>(value);
}

class StringTable final {
public:
    void collect(const std::string_view value) {
        if (indexes_.contains(value)) return;
        const std::uint32_t index = checked_count(values_.size(), "string table");
        auto [found, inserted] = indexes_.emplace(std::string(value), index);
        if (inserted) values_.push_back(&found->first);
    }

    void collect(const JsonValue& value) {
        if (const std::string* string = value.string(); string != nullptr) {
            collect(*string);
            return;
        }
        if (const JsonValue::Array* array = value.array(); array != nullptr) {
            for (const JsonValue& child : *array) collect(child);
            return;
        }
        if (const JsonValue::Object* object = value.object(); object != nullptr) {
            for (const auto& [key, child] : *object) {
                collect(key);
                collect(child);
            }
        }
    }

    void collect(const CompiledSourceMap& source_map) {
        collect(source_map.source_id);
        for (const CompiledSourceMapEntry& entry : source_map.entries) {
            collect(entry.path);
            collect(entry.kind);
            if (entry.name.has_value()) collect(*entry.name);
            collect(entry.span.source_id);
            collect(entry.runtime_component_path);
        }
    }

    [[nodiscard]] std::uint32_t index(const std::string_view value) const {
        const auto found = indexes_.find(value);
        if (found == indexes_.end()) {
            throw std::logic_error("compiled artifact string was not interned");
        }
        return found->second;
    }

    void encode(Bytes& output) const {
        write_integer(output, checked_count(values_.size(), "string table"));
        for (const std::string* value : values_) {
            write_integer(output, checked_count(value->size(), "string"));
            output.insert(output.end(), value->begin(), value->end());
        }
    }

private:
    std::map<std::string, std::uint32_t, std::less<>> indexes_;
    std::vector<const std::string*> values_;
};

void encode_value(Bytes& output, const JsonValue& value, const StringTable& strings) {
    std::visit(
        [&output, &strings](const auto& stored) {
            using Stored = std::remove_cvref_t<decltype(stored)>;
            if constexpr (std::is_same_v<Stored, JsonValue::Null>) {
                output.push_back(static_cast<std::uint8_t>(ValueTag::null_value));
            } else if constexpr (std::is_same_v<Stored, bool>) {
                output.push_back(static_cast<std::uint8_t>(
                    stored ? ValueTag::true_value : ValueTag::false_value
                ));
            } else if constexpr (std::is_same_v<Stored, std::int64_t>) {
                output.push_back(static_cast<std::uint8_t>(ValueTag::integer));
                write_integer(output, std::bit_cast<std::uint64_t>(stored));
            } else if constexpr (std::is_same_v<Stored, double>) {
                output.push_back(static_cast<std::uint8_t>(ValueTag::number));
                write_integer(output, std::bit_cast<std::uint64_t>(stored));
            } else if constexpr (std::is_same_v<Stored, std::string>) {
                output.push_back(static_cast<std::uint8_t>(ValueTag::string));
                write_integer(output, strings.index(stored));
            } else if constexpr (std::is_same_v<Stored, JsonValue::Array>) {
                output.push_back(static_cast<std::uint8_t>(ValueTag::array));
                write_integer(output, checked_count(stored.size(), "JSON array"));
                for (const JsonValue& child : stored) encode_value(output, child, strings);
            } else if constexpr (std::is_same_v<Stored, JsonValue::Object>) {
                output.push_back(static_cast<std::uint8_t>(ValueTag::object));
                write_integer(output, checked_count(stored.size(), "JSON object"));
                for (const auto& [key, child] : stored) {
                    write_integer(output, strings.index(key));
                    encode_value(output, child, strings);
                }
            }
        },
        value.storage()
    );
}

class Reader final {
public:
    explicit Reader(std::shared_ptr<const Bytes> storage)
        : storage_(std::move(storage)), bytes_(*storage_) {}

    void header() {
        const std::span<const std::uint8_t> encoded_magic = take(magic.size());
        if (!std::ranges::equal(encoded_magic, magic)) {
            throw std::runtime_error("compiled module artifact magic is invalid");
        }
        if (integer<std::uint32_t>() != compiled_module_artifact_version) {
            throw std::runtime_error("compiled module artifact version is unsupported");
        }
        const std::uint32_t count = integer<std::uint32_t>();
        if (count > 1'000'000U) {
            throw std::runtime_error("compiled module artifact string table exceeds its limit");
        }
        strings_.reserve(count);
        for (std::uint32_t index = 0U; index < count; ++index) {
            const std::uint32_t size = integer<std::uint32_t>();
            const std::span<const std::uint8_t> encoded = take(size);
            const std::string_view value(
                reinterpret_cast<const char*>(encoded.data()),
                encoded.size()
            );
            if (!core::valid_utf8(value)) {
                throw std::runtime_error("compiled module artifact string is not valid UTF-8");
            }
            strings_.push_back(value);
        }

        const std::size_t remaining_bytes = bytes_.size() - offset_;
        nodes_.reserve(std::min<std::size_t>(1'000'000U, remaining_bytes / 8U));
        object_items_.reserve(std::min<std::size_t>(1'000'000U, remaining_bytes / 12U));
        array_items_.reserve(std::min<std::size_t>(100'000U, remaining_bytes / 64U));
    }

    [[nodiscard]] std::uint32_t value(const std::size_t depth = 0U) {
        if (depth > 256U || ++value_count_ > 1'000'000U ||
            nodes_.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("compiled module artifact JSON exceeds its structural limit");
        }
        const std::uint32_t node_index = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(data::FrozenJsonNode{});
        const ValueTag tag = static_cast<ValueTag>(byte());
        switch (tag) {
        case ValueTag::null_value:
            nodes_[node_index] = data::FrozenJsonNode(data::JsonViewKind::null_value);
            break;
        case ValueTag::false_value:
        case ValueTag::true_value:
            nodes_[node_index] = data::FrozenJsonNode(
                data::JsonViewKind::boolean,
                tag == ValueTag::true_value ? 1U : 0U
            );
            break;
        case ValueTag::integer:
            nodes_[node_index] = data::FrozenJsonNode(
                data::JsonViewKind::integer,
                integer<std::uint64_t>()
            );
            break;
        case ValueTag::number: {
            const std::uint64_t bits = integer<std::uint64_t>();
            if (!std::isfinite(std::bit_cast<double>(bits))) {
                throw std::runtime_error("compiled module artifact number is not finite");
            }
            nodes_[node_index] = data::FrozenJsonNode(data::JsonViewKind::number, bits);
            break;
        }
        case ValueTag::string:
            nodes_[node_index] = data::FrozenJsonNode(
                data::JsonViewKind::string,
                string_index()
            );
            break;
        case ValueTag::array: {
            const std::uint32_t count = integer<std::uint32_t>();
            if (count > 100'000U || array_items_.size() >
                    std::numeric_limits<std::uint32_t>::max() - count) {
                throw std::runtime_error("compiled module artifact array exceeds its structural limit");
            }
            const std::uint32_t first = static_cast<std::uint32_t>(array_items_.size());
            array_items_.resize(array_items_.size() + count);
            nodes_[node_index] = data::FrozenJsonNode{
                data::JsonViewKind::array, 0U, first, count,
            };
            for (std::uint32_t index = 0U; index < count; ++index) {
                const std::uint32_t child = value(depth + 1U);
                array_items_[static_cast<std::size_t>(first) + index] = child;
            }
            break;
        }
        case ValueTag::object: {
            const std::uint32_t count = integer<std::uint32_t>();
            if (count > 100'000U || object_items_.size() >
                    std::numeric_limits<std::uint32_t>::max() - count) {
                throw std::runtime_error("compiled module artifact object exceeds its structural limit");
            }
            const std::uint32_t first = static_cast<std::uint32_t>(object_items_.size());
            object_items_.resize(object_items_.size() + count);
            nodes_[node_index] = data::FrozenJsonNode{
                data::JsonViewKind::object, 0U, first, count,
            };
            constexpr std::uint32_t linear_duplicate_check_limit = 16U;
            std::set<std::string_view> wide_keys;
            for (std::uint32_t index = 0U; index < count; ++index) {
                const std::uint32_t key = string_index();
                const std::string_view name = strings_[key];
                bool duplicate = false;
                if (count <= linear_duplicate_check_limit) {
                    for (std::uint32_t previous = 0U; previous < index; ++previous) {
                        const std::uint32_t previous_key = object_items_[
                            static_cast<std::size_t>(first) + previous
                        ].key;
                        if (strings_[previous_key] == name) {
                            duplicate = true;
                            break;
                        }
                    }
                } else {
                    duplicate = !wide_keys.insert(name).second;
                }
                if (duplicate) {
                    throw std::runtime_error(
                        "compiled module artifact object contains duplicate field '" +
                        std::string(name) + "'"
                    );
                }
                const std::uint32_t child = value(depth + 1U);
                object_items_[static_cast<std::size_t>(first) + index] =
                    data::FrozenJsonObjectEntry{key, child};
            }
            break;
        }
        default:
            throw std::runtime_error("compiled module artifact JSON tag is invalid");
        }
        return node_index;
    }

    [[nodiscard]] data::FrozenJsonDocument document(const std::uint32_t root) && {
        return data::FrozenJsonDocument(
            std::move(storage_),
            std::move(strings_),
            std::move(nodes_),
            std::move(array_items_),
            std::move(object_items_),
            root
        );
    }

    [[nodiscard]] CompiledSourceMap source_map() {
        CompiledSourceMap result;
        result.source_id = string();
        const std::uint32_t count = integer<std::uint32_t>();
        if (count > 1'000'000U) {
            throw std::runtime_error("compiled module artifact source map exceeds its limit");
        }
        result.entries.reserve(count);
        for (std::uint32_t index = 0U; index < count; ++index) {
            std::string path = string();
            std::string kind = string();
            const bool has_name = boolean();
            std::optional<std::string> name;
            if (has_name) name = string();
            std::string source_id = string();
            const SourcePosition start = position();
            const SourcePosition end = position();
            if (end.offset < start.offset) {
                throw std::runtime_error(
                    "compiled module artifact source-map range ends before it starts"
                );
            }
            result.entries.push_back(CompiledSourceMapEntry{
                std::move(path),
                std::move(kind),
                std::move(name),
                SourceSpan{
                    std::move(source_id),
                    start,
                    end,
                    end.offset - start.offset,
                    {},
                },
                string(),
            });
        }
        return result;
    }

    void finish() const {
        if (offset_ != bytes_.size()) {
            throw std::runtime_error("compiled module artifact has trailing bytes");
        }
    }

private:
    template <typename Integer>
        requires std::is_unsigned_v<Integer>
    [[nodiscard]] Integer integer() {
        const std::span<const std::uint8_t> encoded = take(sizeof(Integer));
        Integer result = 0U;
        for (std::size_t byte_index = 0U; byte_index < sizeof(Integer); ++byte_index) {
            result |= static_cast<Integer>(encoded[byte_index]) << (byte_index * 8U);
        }
        return result;
    }

    [[nodiscard]] std::uint8_t byte() {
        return take(1U)[0U];
    }

    [[nodiscard]] bool boolean() {
        const std::uint8_t encoded = byte();
        if (encoded > 1U) {
            throw std::runtime_error("compiled module artifact boolean is invalid");
        }
        return encoded != 0U;
    }

    [[nodiscard]] std::uint32_t string_index() {
        const std::uint32_t index = integer<std::uint32_t>();
        if (index >= strings_.size()) {
            throw std::runtime_error("compiled module artifact string reference is invalid");
        }
        return index;
    }

    [[nodiscard]] std::string string() {
        return std::string(strings_[string_index()]);
    }

    [[nodiscard]] SourcePosition position() {
        const std::uint32_t line = integer<std::uint32_t>();
        const std::uint32_t column = integer<std::uint32_t>();
        if (line == 0U || column == 0U) {
            throw std::runtime_error("compiled module artifact source position is invalid");
        }
        return SourcePosition{line, column, integer<std::uint64_t>()};
    }

    [[nodiscard]] std::span<const std::uint8_t> take(const std::size_t count) {
        if (count > bytes_.size() - offset_) {
            throw std::runtime_error("compiled module artifact is truncated");
        }
        const std::span<const std::uint8_t> result = bytes_.subspan(offset_, count);
        offset_ += count;
        return result;
    }

    std::shared_ptr<const Bytes> storage_;
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
    std::size_t value_count_ = 0U;
    std::vector<std::string_view> strings_;
    std::vector<data::FrozenJsonNode> nodes_;
    std::vector<std::uint32_t> array_items_;
    std::vector<data::FrozenJsonObjectEntry> object_items_;
};

} // namespace

data::JsonValue source_map_entry_json(const CompiledSourceMapEntry& entry) {
    return JsonValue(JsonValue::Object{
        {"path", JsonValue(entry.path)},
        {"kind", JsonValue(entry.kind)},
        {"name", entry.name.has_value() ? JsonValue(*entry.name) : JsonValue(JsonValue::Null{})},
        {"sourceId", JsonValue(entry.span.source_id)},
        {"range", JsonValue(JsonValue::Object{
            {"byteStart", JsonValue(static_cast<std::int64_t>(entry.span.start.offset))},
            {"byteEnd", JsonValue(static_cast<std::int64_t>(entry.span.end.offset))},
            {"lineStart", JsonValue(static_cast<std::int64_t>(entry.span.start.line))},
            {"columnStart", JsonValue(static_cast<std::int64_t>(entry.span.start.column))},
            {"lineEnd", JsonValue(static_cast<std::int64_t>(entry.span.end.line))},
            {"columnEnd", JsonValue(static_cast<std::int64_t>(entry.span.end.column))},
        })},
        {"runtimeComponentPath", JsonValue(entry.runtime_component_path)},
    });
}

std::vector<std::uint8_t> encode_compiled_module_artifact(
    const data::JsonValue& unit,
    const CompiledSourceMap& source_map
) {
    StringTable strings;
    strings.collect(unit);
    strings.collect(source_map);

    Bytes output;
    output.reserve(512U * 1'024U);
    output.insert(output.end(), magic.begin(), magic.end());
    write_integer(
        output,
        compiled_module_artifact_version
    );
    strings.encode(output);
    encode_value(output, unit, strings);
    write_integer(output, strings.index(source_map.source_id));
    write_integer(output, checked_count(source_map.entries.size(), "source-map entry count"));
    for (const CompiledSourceMapEntry& entry : source_map.entries) {
        write_integer(output, strings.index(entry.path));
        write_integer(output, strings.index(entry.kind));
        output.push_back(entry.name.has_value() ? std::uint8_t{1U} : std::uint8_t{0U});
        if (entry.name.has_value()) write_integer(output, strings.index(*entry.name));
        write_integer(output, strings.index(entry.span.source_id));
        write_integer(output, entry.span.start.line);
        write_integer(output, entry.span.start.column);
        write_integer(output, entry.span.start.offset);
        write_integer(output, entry.span.end.line);
        write_integer(output, entry.span.end.column);
        write_integer(output, entry.span.end.offset);
        write_integer(output, strings.index(entry.runtime_component_path));
    }
    return output;
}

CompiledModuleArtifact decode_compiled_module_artifact(
    const std::span<const std::uint8_t> artifact
) {
    auto storage = std::make_shared<const Bytes>(artifact.begin(), artifact.end());
    Reader reader(std::move(storage));
    reader.header();
    const std::uint32_t root = reader.value();
    CompiledSourceMap source_map = reader.source_map();
    reader.finish();
    return CompiledModuleArtifact{
        std::move(reader).document(root),
        std::move(source_map),
    };
}

} // namespace strata::compiler
