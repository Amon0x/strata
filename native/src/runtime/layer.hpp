#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace strata::runtime {

enum class LayerRole { screen, overlay };

struct LayerSnapshot final {
    std::string id;
    LayerRole role;
    std::optional<std::string> transition;

    [[nodiscard]] friend bool operator==(const LayerSnapshot&, const LayerSnapshot&) = default;
};

/** Ordered screen stack plus insertion-ordered overlays, independent of retained node storage. */
class LayerStack final {
public:
    using Invalidation = std::function<void()>;

    explicit LayerStack(Invalidation invalidation = {});

    void push_screen(std::string id, std::optional<std::string> transition = std::nullopt);
    void replace_screen(std::string id, std::optional<std::string> transition = std::nullopt);
    [[nodiscard]] std::optional<std::string> pop_screen();
    [[nodiscard]] bool show_overlay(
        std::string id,
        std::optional<std::string> transition = std::nullopt
    );
    [[nodiscard]] bool hide_overlay(std::string_view id);
    [[nodiscard]] bool clear();
    /** Removes layers no longer declared by a newly activated unit, preserving surviving order. */
    [[nodiscard]] bool retain(const std::set<std::string, std::less<>>& ids);

    [[nodiscard]] std::optional<std::string_view> active_screen() const noexcept;
    [[nodiscard]] bool root_replaced() const noexcept;
    [[nodiscard]] std::vector<LayerSnapshot> snapshot() const;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    struct Entry final {
        std::string id;
        std::optional<std::string> transition;
    };

    void changed();
    static void validate_id(std::string_view id);

    std::vector<Entry> screens_;
    std::vector<Entry> overlays_;
    bool root_replaced_ = false;
    Invalidation invalidation_;
    std::uint64_t generation_ = 0U;
};

enum class DeclarativeLayerOperation { push, replace, pop, show, hide };
enum class LayerOperationStatus { handled, ignored, failed };

struct LayerOperationResult final {
    LayerOperationStatus status;
    std::optional<std::string> message;
};

class DeclarativeLayerRegistry final {
public:
    void register_screen(std::string name, std::string id);
    void register_overlay(std::string name, std::string id);
    [[nodiscard]] bool unregister_screen(std::string_view name);
    [[nodiscard]] bool unregister_overlay(std::string_view name);
    [[nodiscard]] LayerOperationResult execute(
        LayerStack& stack,
        DeclarativeLayerOperation operation,
        std::optional<std::string_view> name = std::nullopt,
        std::optional<std::string_view> transition = std::nullopt
    ) const;

private:
    std::map<std::string, std::string, std::less<>> screens_;
    std::map<std::string, std::string, std::less<>> overlays_;
};

} // namespace strata::runtime
