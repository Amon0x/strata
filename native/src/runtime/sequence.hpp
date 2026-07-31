#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "runtime/value.hpp"

namespace strata::runtime {

/**
 * Immutable keyed index space whose generation changes whenever count, ordering, or keys change.
 * Consumers retain this provider and query only the indexes or keys they actually observe.
 */
class KeyedSequence {
public:
    virtual ~KeyedSequence() = default;

    [[nodiscard]] virtual std::uint64_t generation() const noexcept = 0;
    [[nodiscard]] virtual std::size_t count() const noexcept = 0;
    [[nodiscard]] virtual std::string key_at(std::size_t index) const = 0;
    [[nodiscard]] virtual std::optional<std::size_t> index_of_key(
        std::string_view key
    ) const = 0;
    /** Exact generation equivalence; snapshot adapters may compare content after a token miss. */
    [[nodiscard]] virtual bool same_generation(const KeyedSequence& other) const noexcept {
        return generation() == other.generation() && count() == other.count();
    }
};

/** Immutable keyed index space which can also retrieve its data item without materializing a row. */
class IndexableSequence : public KeyedSequence {
public:
    [[nodiscard]] virtual const Value& item_at(std::size_t index) const = 0;
    /** Original provider index, preserved when a filtered sequence compacts its visible indexes. */
    [[nodiscard]] virtual std::size_t source_index_at(std::size_t index) const {
        return index;
    }
};

} // namespace strata::runtime
