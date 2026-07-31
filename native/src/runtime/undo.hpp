#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/state.hpp"

namespace strata::runtime {

struct UndoRecordOptions final {
    std::string label;
    std::optional<std::string> coalesce_key;
    std::int64_t timestamp_nanos = 0;
};

struct UndoStackStatus final {
    bool can_undo = false;
    bool can_redo = false;
    std::optional<std::string> undo_label;
    std::optional<std::string> redo_label;
};

/** Per-surface bounded application undo over the existing immutable StateStore snapshots. */
class UndoManager final {
public:
    using InvalidationDiagnostic = std::function<void(
        std::string_view scope,
        std::string_view action_id
    )>;

    explicit UndoManager(InvalidationDiagnostic invalidation_diagnostic = {});

    void record(
        std::string scope,
        StateSnapshot before,
        StateSnapshot after,
        UndoRecordOptions options
    );
    /** Groups every record until end_group into one user-visible entry. */
    void begin_group(std::string scope, std::string label = {});
    void end_group(std::string_view scope, bool commit = true);

    [[nodiscard]] bool undo(std::string_view scope, StateStore& state);
    [[nodiscard]] bool redo(std::string_view scope, StateStore& state);
    void invalidate(std::string_view scope, std::string_view action_id);
    void invalidate_other_scopes(std::string_view preserved_scope, std::string_view action_id);
    void clear(std::string_view scope);
    void clear_all() noexcept;
    [[nodiscard]] UndoStackStatus status(std::string_view scope) const;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    struct Entry final {
        StateSnapshot before;
        StateSnapshot after;
        std::string label;
        std::optional<std::string> coalesce_key;
        std::int64_t timestamp_nanos = 0;
    };
    struct Stack final {
        std::vector<Entry> undo;
        std::vector<Entry> redo;
        std::size_t group_depth = 0U;
        std::optional<Entry> pending_group;
        std::string group_label;
    };

    void append(Stack& stack, Entry entry);

    std::map<std::string, Stack, std::less<>> stacks_;
    InvalidationDiagnostic invalidation_diagnostic_;
    std::uint64_t generation_ = 0U;
};

} // namespace strata::runtime
