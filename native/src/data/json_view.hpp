#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "data/json.hpp"

namespace strata::data {

enum class JsonViewKind : std::uint8_t {
    invalid,
    null_value,
    boolean,
    integer,
    number,
    string,
    array,
    object,
};

struct FrozenJsonNode final {
    constexpr FrozenJsonNode() = default;
    constexpr FrozenJsonNode(
        const JsonViewKind kind,
        const std::uint64_t scalar = 0U,
        const std::uint32_t first = 0U,
        const std::uint32_t count = 0U
    ) noexcept : scalar(scalar), first(first), count_and_kind_(
        count | (static_cast<std::uint32_t>(kind) << kind_shift)
    ) {}

    [[nodiscard]] constexpr JsonViewKind kind() const noexcept {
        return static_cast<JsonViewKind>(count_and_kind_ >> kind_shift);
    }
    [[nodiscard]] constexpr std::uint32_t count() const noexcept {
        return count_and_kind_ & count_mask;
    }

    std::uint64_t scalar = 0U;
    std::uint32_t first = 0U;

private:
    static constexpr std::uint32_t kind_shift = 28U;
    static constexpr std::uint32_t count_mask = (1U << kind_shift) - 1U;
    std::uint32_t count_and_kind_ = 0U;
};

static_assert(sizeof(FrozenJsonNode) == 16U);

struct FrozenJsonObjectEntry final {
    std::uint32_t key = 0U;
    std::uint32_t value = 0U;
};

class JsonView;
class JsonArrayView;
class JsonObjectView;

/**
 * Immutable JSON document backed by one owned encoded-artifact blob plus flat node/edge indexes.
 * Strings remain views into the blob; decoding performs no per-value or per-string allocation.
 */
class FrozenJsonDocument final {
public:
    FrozenJsonDocument() = default;
    FrozenJsonDocument(
        std::shared_ptr<const std::vector<std::uint8_t>> storage,
        std::vector<std::string_view> strings,
        std::vector<FrozenJsonNode> nodes,
        std::vector<std::uint32_t> array_items,
        std::vector<FrozenJsonObjectEntry> object_items,
        std::uint32_t root
    );

    [[nodiscard]] JsonView root() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    friend class JsonView;
    friend class JsonArrayView;
    friend class JsonObjectView;

    std::shared_ptr<const std::vector<std::uint8_t>> storage_;
    std::vector<std::string_view> strings_;
    std::vector<FrozenJsonNode> nodes_;
    std::vector<std::uint32_t> array_items_;
    std::vector<FrozenJsonObjectEntry> object_items_;
    std::uint32_t root_ = 0U;
};

/** Cheap non-owning view over either ordinary owned JSON or a frozen artifact document. */
class JsonView final {
public:
    JsonView() = default;
    JsonView(const JsonValue& value) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] JsonViewKind kind() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] std::optional<bool> boolean() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> integer() const noexcept;
    [[nodiscard]] std::optional<double> number() const noexcept;
    [[nodiscard]] std::optional<std::string_view> string() const noexcept;
    [[nodiscard]] std::optional<JsonArrayView> array() const noexcept;
    [[nodiscard]] std::optional<JsonObjectView> object() const noexcept;
    [[nodiscard]] JsonView find(std::string_view key) const noexcept;
    [[nodiscard]] friend bool operator==(
        const JsonView& left,
        const JsonView& right
    ) noexcept {
        if (left.node_ != right.node_) return false;
        return left.node_ == owned_node
            ? left.owned_ == right.owned_
            : left.frozen_ == right.frozen_;
    }

private:
    friend class FrozenJsonDocument;
    friend class JsonArrayView;
    friend class JsonObjectView;

    JsonView(const FrozenJsonDocument& document, std::uint32_t node) noexcept;

    static constexpr std::uint32_t owned_node = std::numeric_limits<std::uint32_t>::max();
    union {
        const JsonValue* owned_ = nullptr;
        const FrozenJsonDocument* frozen_;
    };
    std::uint32_t node_ = owned_node;
};

class JsonArrayView final {
public:
    class Iterator final {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = JsonView;
        using iterator_category = std::random_access_iterator_tag;

        [[nodiscard]] JsonView operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept;
        Iterator& operator--() noexcept;
        Iterator& operator+=(difference_type count) noexcept;
        Iterator& operator-=(difference_type count) noexcept;
        [[nodiscard]] JsonView operator[](difference_type offset) const noexcept;
        [[nodiscard]] friend Iterator operator+(Iterator value, difference_type count) noexcept {
            value += count;
            return value;
        }
        [[nodiscard]] friend Iterator operator+(difference_type count, Iterator value) noexcept {
            value += count;
            return value;
        }
        [[nodiscard]] friend Iterator operator-(Iterator value, difference_type count) noexcept {
            value -= count;
            return value;
        }
        [[nodiscard]] friend difference_type operator-(const Iterator& left, const Iterator& right) noexcept {
            return static_cast<difference_type>(left.index_) - static_cast<difference_type>(right.index_);
        }
        [[nodiscard]] friend auto operator<=>(const Iterator&, const Iterator&) noexcept = default;

    private:
        friend class JsonArrayView;
        Iterator(const JsonArrayView* owner, std::size_t index) noexcept;
        const JsonArrayView* owner_ = nullptr;
        std::size_t index_ = 0U;
    };

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] JsonView operator[](std::size_t index) const noexcept;
    [[nodiscard]] JsonView front() const noexcept;
    [[nodiscard]] JsonView back() const noexcept;
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;

private:
    friend class JsonView;
    explicit JsonArrayView(const JsonValue::Array& value) noexcept;
    JsonArrayView(const FrozenJsonDocument& document, std::uint32_t first, std::uint32_t count) noexcept;

    static constexpr std::uint32_t owned_range = std::numeric_limits<std::uint32_t>::max();
    union {
        const JsonValue::Array* owned_ = nullptr;
        const FrozenJsonDocument* frozen_;
    };
    std::uint32_t first_ = owned_range;
    std::uint32_t count_ = 0U;
};

class JsonObjectView final {
public:
    using Entry = std::pair<std::string_view, JsonView>;

    class Iterator final {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = Entry;
        using iterator_category = std::forward_iterator_tag;

        [[nodiscard]] Entry operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept;
        [[nodiscard]] friend bool operator==(const Iterator&, const Iterator&) noexcept = default;

    private:
        friend class JsonObjectView;
        Iterator(const JsonObjectView* owner, std::size_t index) noexcept;
        const JsonObjectView* owner_ = nullptr;
        std::size_t index_ = 0U;
    };

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Entry operator[](std::size_t index) const noexcept;
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;

private:
    friend class JsonView;
    explicit JsonObjectView(const JsonValue::Object& value) noexcept;
    JsonObjectView(const FrozenJsonDocument& document, std::uint32_t first, std::uint32_t count) noexcept;

    static constexpr std::uint32_t owned_range = std::numeric_limits<std::uint32_t>::max();
    union {
        const JsonValue::Object* owned_ = nullptr;
        const FrozenJsonDocument* frozen_;
    };
    std::uint32_t first_ = owned_range;
    std::uint32_t count_ = 0U;
};

inline JsonView FrozenJsonDocument::root() const noexcept {
    return empty() ? JsonView{} : JsonView(*this, root_);
}

inline bool FrozenJsonDocument::empty() const noexcept {
    return storage_ == nullptr || nodes_.empty() || root_ >= nodes_.size();
}

inline JsonView::JsonView(const JsonValue& value) noexcept
    : owned_(&value), node_(owned_node) {}

inline JsonView::JsonView(
    const FrozenJsonDocument& document,
    const std::uint32_t node
) noexcept : frozen_(&document), node_(node) {}

inline bool JsonView::valid() const noexcept {
    return node_ == owned_node
        ? owned_ != nullptr
        : frozen_ != nullptr && node_ < frozen_->nodes_.size();
}

inline JsonView::operator bool() const noexcept { return valid(); }

inline JsonViewKind JsonView::kind() const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return JsonViewKind::invalid;
        const JsonValue::Storage& storage = owned_->storage();
        if (std::holds_alternative<JsonValue::Null>(storage)) return JsonViewKind::null_value;
        if (std::holds_alternative<bool>(storage)) return JsonViewKind::boolean;
        if (std::holds_alternative<std::int64_t>(storage)) return JsonViewKind::integer;
        if (std::holds_alternative<double>(storage)) return JsonViewKind::number;
        if (std::holds_alternative<std::string>(storage)) return JsonViewKind::string;
        if (std::holds_alternative<JsonValue::Array>(storage)) return JsonViewKind::array;
        return JsonViewKind::object;
    }
    return valid() ? frozen_->nodes_[node_].kind() : JsonViewKind::invalid;
}

inline bool JsonView::is_null() const noexcept { return kind() == JsonViewKind::null_value; }

inline std::optional<bool> JsonView::boolean() const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return std::nullopt;
        const bool* value = owned_->boolean();
        return value != nullptr ? std::optional<bool>(*value) : std::nullopt;
    }
    if (kind() != JsonViewKind::boolean) return std::nullopt;
    return frozen_->nodes_[node_].scalar != 0U;
}

inline std::optional<std::int64_t> JsonView::integer() const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return std::nullopt;
        const std::int64_t* value = owned_->integer();
        return value != nullptr ? std::optional<std::int64_t>(*value) : std::nullopt;
    }
    if (kind() != JsonViewKind::integer) return std::nullopt;
    return std::bit_cast<std::int64_t>(frozen_->nodes_[node_].scalar);
}

inline std::optional<double> JsonView::number() const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return std::nullopt;
        const double* value = owned_->number();
        return value != nullptr ? std::optional<double>(*value) : std::nullopt;
    }
    if (kind() != JsonViewKind::number) return std::nullopt;
    return std::bit_cast<double>(frozen_->nodes_[node_].scalar);
}

inline std::optional<std::string_view> JsonView::string() const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return std::nullopt;
        const std::string* value = owned_->string();
        return value != nullptr ? std::optional<std::string_view>(*value) : std::nullopt;
    }
    if (kind() != JsonViewKind::string) return std::nullopt;
    const std::uint64_t index = frozen_->nodes_[node_].scalar;
    return index < frozen_->strings_.size()
        ? std::optional<std::string_view>(frozen_->strings_[static_cast<std::size_t>(index)])
        : std::nullopt;
}

inline std::optional<JsonArrayView> JsonView::array() const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return std::nullopt;
        const JsonValue::Array* value = owned_->array();
        return value != nullptr ? std::optional<JsonArrayView>(JsonArrayView(*value)) : std::nullopt;
    }
    if (kind() != JsonViewKind::array) return std::nullopt;
    const FrozenJsonNode& node = frozen_->nodes_[node_];
    return JsonArrayView(*frozen_, node.first, node.count());
}

inline std::optional<JsonObjectView> JsonView::object() const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return std::nullopt;
        const JsonValue::Object* value = owned_->object();
        return value != nullptr ? std::optional<JsonObjectView>(JsonObjectView(*value)) : std::nullopt;
    }
    if (kind() != JsonViewKind::object) return std::nullopt;
    const FrozenJsonNode& node = frozen_->nodes_[node_];
    return JsonObjectView(*frozen_, node.first, node.count());
}

inline JsonView JsonView::find(const std::string_view key) const noexcept {
    if (node_ == owned_node) {
        if (owned_ == nullptr) return {};
        const JsonValue* value = owned_->find(key);
        return value != nullptr ? JsonView(*value) : JsonView{};
    }
    if (!valid() || frozen_->nodes_[node_].kind() != JsonViewKind::object) return {};
    const FrozenJsonNode& object = frozen_->nodes_[node_];
    for (std::uint32_t index = 0U; index < object.count(); ++index) {
        const FrozenJsonObjectEntry& entry = frozen_->object_items_[object.first + index];
        if (frozen_->strings_[entry.key] == key) return JsonView(*frozen_, entry.value);
    }
    return {};
}

inline JsonArrayView::JsonArrayView(const JsonValue::Array& value) noexcept
    : owned_(&value), first_(owned_range) {}

inline JsonArrayView::JsonArrayView(
    const FrozenJsonDocument& document,
    const std::uint32_t first,
    const std::uint32_t count
) noexcept : frozen_(&document), first_(first), count_(count) {}

inline std::size_t JsonArrayView::size() const noexcept {
    return first_ == owned_range ? owned_->size() : count_;
}

inline bool JsonArrayView::empty() const noexcept { return size() == 0U; }

inline JsonView JsonArrayView::operator[](const std::size_t index) const noexcept {
    if (index >= size()) return {};
    if (first_ == owned_range) return JsonView((*owned_)[index]);
    const std::size_t edge = static_cast<std::size_t>(first_) + index;
    return edge < frozen_->array_items_.size()
        ? JsonView(*frozen_, frozen_->array_items_[edge])
        : JsonView{};
}

inline JsonView JsonArrayView::front() const noexcept { return (*this)[0U]; }
inline JsonView JsonArrayView::back() const noexcept {
    return empty() ? JsonView{} : (*this)[size() - 1U];
}
inline JsonArrayView::Iterator JsonArrayView::begin() const noexcept { return Iterator(this, 0U); }
inline JsonArrayView::Iterator JsonArrayView::end() const noexcept { return Iterator(this, size()); }

inline JsonArrayView::Iterator::Iterator(
    const JsonArrayView* const owner,
    const std::size_t index
) noexcept : owner_(owner), index_(index) {}
inline JsonView JsonArrayView::Iterator::operator*() const noexcept { return (*owner_)[index_]; }
inline JsonArrayView::Iterator& JsonArrayView::Iterator::operator++() noexcept {
    ++index_;
    return *this;
}
inline JsonArrayView::Iterator JsonArrayView::Iterator::operator++(int) noexcept {
    auto copy = *this;
    ++*this;
    return copy;
}
inline JsonArrayView::Iterator& JsonArrayView::Iterator::operator--() noexcept {
    --index_;
    return *this;
}
inline JsonArrayView::Iterator& JsonArrayView::Iterator::operator+=(
    const difference_type count
) noexcept {
    index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + count);
    return *this;
}
inline JsonArrayView::Iterator& JsonArrayView::Iterator::operator-=(
    const difference_type count
) noexcept {
    return *this += -count;
}
inline JsonView JsonArrayView::Iterator::operator[](const difference_type offset) const noexcept {
    return (*owner_)[static_cast<std::size_t>(static_cast<difference_type>(index_) + offset)];
}

inline JsonObjectView::JsonObjectView(const JsonValue::Object& value) noexcept
    : owned_(&value), first_(owned_range) {}

inline JsonObjectView::JsonObjectView(
    const FrozenJsonDocument& document,
    const std::uint32_t first,
    const std::uint32_t count
) noexcept : frozen_(&document), first_(first), count_(count) {}

inline std::size_t JsonObjectView::size() const noexcept {
    return first_ == owned_range ? owned_->size() : count_;
}

inline bool JsonObjectView::empty() const noexcept { return size() == 0U; }

inline JsonObjectView::Entry JsonObjectView::operator[](const std::size_t index) const noexcept {
    if (index >= size()) return {};
    if (first_ == owned_range) {
        const JsonValue::ObjectEntry& entry = (*owned_)[index];
        return Entry{entry.first, JsonView(entry.second)};
    }
    const std::size_t edge_index = static_cast<std::size_t>(first_) + index;
    if (edge_index >= frozen_->object_items_.size()) return {};
    const FrozenJsonObjectEntry& entry = frozen_->object_items_[edge_index];
    if (entry.key >= frozen_->strings_.size()) return {};
    return Entry{frozen_->strings_[entry.key], JsonView(*frozen_, entry.value)};
}

inline JsonObjectView::Iterator JsonObjectView::begin() const noexcept { return Iterator(this, 0U); }
inline JsonObjectView::Iterator JsonObjectView::end() const noexcept { return Iterator(this, size()); }

inline JsonObjectView::Iterator::Iterator(
    const JsonObjectView* const owner,
    const std::size_t index
) noexcept : owner_(owner), index_(index) {}
inline JsonObjectView::Entry JsonObjectView::Iterator::operator*() const noexcept {
    return (*owner_)[index_];
}
inline JsonObjectView::Iterator& JsonObjectView::Iterator::operator++() noexcept {
    ++index_;
    return *this;
}
inline JsonObjectView::Iterator JsonObjectView::Iterator::operator++(int) noexcept {
    auto copy = *this;
    ++*this;
    return copy;
}

static_assert(sizeof(JsonView) == 16U);
static_assert(sizeof(JsonArrayView) == 16U);
static_assert(sizeof(JsonObjectView) == 16U);

[[nodiscard]] JsonValue materialize_json(JsonView value);

} // namespace strata::data
