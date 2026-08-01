#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "data/json.hpp"
#include "runtime/value.hpp"
#include "runtime/value_schema.hpp"

namespace strata::runtime {

using HostSchemaRoots = std::map<std::string, ValueSchemaPtr, std::less<>>;

class HostNode;
struct HostListData;
struct HostObjectData;
struct HostScalarData;

using HostEvaluationCounter = std::shared_ptr<std::atomic<std::uint64_t>>;

/**
 * Immutable snapshot scalars live directly in their HostNode. This avoids one shared allocation,
 * function wrapper, mutex, and JSON copy for every scalar in large host collections while
 * retaining exact once-only lazy-evaluation accounting.
 */
struct HostConstantData final {
    HostConstantData(Value value, HostEvaluationCounter counter);
    HostConstantData(const HostConstantData&) = delete;
    HostConstantData& operator=(const HostConstantData&) = delete;
    HostConstantData(HostConstantData&& other);
    HostConstantData& operator=(HostConstantData&& other);

    Value value;
    HostEvaluationCounter counter;
    mutable std::atomic<bool> evaluated = false;
};

enum class HostNodeKind { null_value, scalar, list, object };

enum class HostPathSegmentKind { field, lookup };

/** One structural host read segment; lookup mirrors DSL [] semantics for lists and objects. */
struct HostPathSegment final {
    HostPathSegmentKind kind = HostPathSegmentKind::field;
    std::string field;
    std::optional<std::size_t> index;

    [[nodiscard]] static HostPathSegment named(std::string field);
    [[nodiscard]] static HostPathSegment lookup(
        std::string field,
        std::optional<std::size_t> index
    );
    [[nodiscard]] friend bool operator==(const HostPathSegment&, const HostPathSegment&) = default;
};

/**
 * One immutable node in a host snapshot. Scalar providers run at most once and only after their
 * exact path is read. Object traversal never evaluates siblings.
 */
class HostNode final {
public:
    using EvaluationCounter = HostEvaluationCounter;

    HostNode() noexcept;
    static HostNode scalar(std::function<Value()> provider, EvaluationCounter counter = {});
    static HostNode constant(Value value, EvaluationCounter counter = {});
    static HostNode list(std::vector<HostNode> items);
    static HostNode object(std::vector<std::pair<std::string, HostNode>> fields);
    static HostNode from_json(const data::JsonValue& value, EvaluationCounter counter = {});

    [[nodiscard]] HostNodeKind kind() const noexcept;
    [[nodiscard]] const HostNode* field(std::string_view name) const noexcept;
    [[nodiscard]] const HostNode* item(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Value materialize() const;

private:
    using Storage = std::variant<
        NullValue,
        std::shared_ptr<HostScalarData>,
        HostConstantData,
        std::shared_ptr<const HostListData>,
        std::shared_ptr<const HostObjectData>
    >;

    explicit HostNode(Storage storage);
    Storage storage_;
};

struct HostListData final {
    std::vector<HostNode> items;
};

struct HostObjectData final {
    std::vector<std::pair<std::string, HostNode>> fields;
};

struct HostResolution final {
    Value value;
    std::string snapshot_id;
    std::uint64_t snapshot_generation = 0U;
};

class HostSnapshot final {
public:
    HostSnapshot(std::string id, std::uint64_t generation, HostNode roots);

    static std::shared_ptr<const HostSnapshot> from_json(
        std::string id,
        std::uint64_t generation,
        const data::JsonValue& roots
    );
    static std::shared_ptr<const HostSnapshot> from_json(
        std::string id,
        std::uint64_t generation,
        const data::JsonValue& roots,
        const HostSchemaRoots& schemas
    );

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::optional<Value> resolve(std::string_view dotted_path) const;
    [[nodiscard]] std::optional<Value> resolve(std::span<const std::string_view> path) const;
    /** Resolves only the addressed leaf; unrelated object/list siblings stay unevaluated. */
    [[nodiscard]] std::optional<Value> resolve(std::span<const HostPathSegment> path) const;
    /** Structural presence check which never evaluates the selected scalar/composite value. */
    [[nodiscard]] bool contains(std::span<const HostPathSegment> path) const;
    [[nodiscard]] std::uint64_t evaluated_scalar_count() const noexcept;

private:
    std::string id_;
    std::uint64_t generation_;
    HostNode roots_;
    HostNode::EvaluationCounter evaluation_counter_;
};

/** Adopts whole immutable generations and emits one invalidation per genuinely newer snapshot. */
class HostStore final {
public:
    using Invalidation = std::function<void(std::uint64_t)>;

    explicit HostStore(Invalidation invalidation = {});

    [[nodiscard]] bool adopt(std::shared_ptr<const HostSnapshot> snapshot);
    [[nodiscard]] const std::shared_ptr<const HostSnapshot>& snapshot() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> generation(std::string_view id) const noexcept;
    /** Immutable roots in adoption order; later snapshots shadow duplicate root names. */
    [[nodiscard]] std::vector<std::shared_ptr<const HostSnapshot>> snapshots() const;
    [[nodiscard]] std::optional<Value> resolve(std::string_view dotted_path) const;
    [[nodiscard]] std::optional<Value> resolve(std::span<const HostPathSegment> path) const;
    /** Resolves a value together with the immutable snapshot generation that owns it. */
    [[nodiscard]] std::optional<HostResolution> resolve_with_origin(
        std::span<const HostPathSegment> path
    ) const;
    /** Returns the owning immutable snapshot without materializing the selected value. */
    [[nodiscard]] std::optional<std::pair<std::string, std::uint64_t>> origin(
        std::span<const HostPathSegment> path
    ) const;
    [[nodiscard]] std::uint64_t invalidation_count() const noexcept;

private:
    std::shared_ptr<const HostSnapshot> snapshot_;
    std::map<std::string, std::shared_ptr<const HostSnapshot>, std::less<>> snapshots_;
    std::vector<std::string> adoption_order_;
    Invalidation invalidation_;
    std::uint64_t invalidation_count_ = 0U;
};

} // namespace strata::runtime
