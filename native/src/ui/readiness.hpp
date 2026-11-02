#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "base/pending.hpp"

namespace strata::ui {

class RetainedNode;
class RetainedTree;
class PreparationReadScope;
class InputRouter;
enum class WidgetFrameCost;

enum class ReadinessPhase : std::uint8_t { layout, paint, frame, packet };

/** A retained consumer's preparation dependency, independent of the cache that recorded it. */
struct NodeReadinessRead final {
    std::uint64_t node_identity = 0U;
    base::ReadinessRead dependency;
    ReadinessPhase phase = ReadinessPhase::packet;
};

/** Immutable, sorted by node identity, resource identity and phase; safe to retain for inspection. */
using NodeReadinessSnapshot = std::vector<NodeReadinessRead>;

/** Independently cached operations must replace only their own observations. */
enum class PreparationUse : std::uint8_t {
    measurement, content, foreground, overlay, behavior, detached_behavior, clip, frame, input,
};

/**
 * One retained tree's owner-only preparation observations. Cached operations keep their reads;
 * publication visits only pending or revision-observing consumers. No node pointers or worker
 * callbacks are retained. Frame consumers additionally rearm their owner's widget callback.
 */
class PreparationReads final : public std::enable_shared_from_this<PreparationReads> {
  public:
    void observe(std::uint64_t node, const base::Readiness& resource);
    void observe(std::uint64_t node, std::span<const base::ReadinessRead> resources);
    void erase(std::uint64_t node) noexcept;
    void erase(std::uint64_t node, PreparationUse use) noexcept;
    void invalidate(RetainedTree& tree, InputRouter* input = nullptr);
    [[nodiscard]] bool references(const base::Readiness& resource) const noexcept;
    [[nodiscard]] NodeReadinessSnapshot snapshot(std::uint64_t node) const;

  private:
    friend class PreparationReadScope;
    using Key = std::pair<std::uint64_t, PreparationUse>;
    /** Sleeping frame callbacks retain their cost, so pixel-only adoption cannot rebuild geometry. */
    struct Observation final {
        std::vector<base::ReadinessRead> resources;
        std::optional<WidgetFrameCost> frame_cost;
    };
    void replace(Key key, std::vector<base::ReadinessRead> reads,
                 std::optional<WidgetFrameCost> frame_cost);
    std::map<Key, Observation> reads_;
    std::set<Key> watched_;
    PreparationReadScope* active_ = nullptr;
};

/**
 * Records dependencies of one cached owner operation, including helpers reading other nodes in
 * the same tree. Commit replaces its reads; unwinding leaves the previous cache's observations.
 */
class PreparationReadScope final {
  public:
    PreparationReadScope(const RetainedNode& node, PreparationUse use,
                         std::optional<WidgetFrameCost> frame_cost = std::nullopt);
    ~PreparationReadScope();
    PreparationReadScope(const PreparationReadScope&) = delete;
    PreparationReadScope& operator=(const PreparationReadScope&) = delete;
    void commit();

  private:
    friend class PreparationReads;
    PreparationReads& owner_;
    PreparationReads::Key key_;
    PreparationReadScope* previous_;
    std::optional<WidgetFrameCost> frame_cost_;
    std::vector<base::ReadinessRead> reads_;
    bool active_ = true;
};

} // namespace strata::ui
