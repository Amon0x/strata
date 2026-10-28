#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>
#include <utility>

namespace strata::core {

class Arena final {
public:
    explicit Arena(std::pmr::memory_resource& upstream) noexcept : storage_(&upstream) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    [[nodiscard]] void* allocate(const std::size_t size, const std::size_t alignment) {
        return storage_.allocate(size, alignment);
    }

    template <typename Value, typename... Arguments>
    [[nodiscard]] Value* create(Arguments&&... arguments) {
        void* storage = allocate(sizeof(Value), alignof(Value));
        return std::construct_at(static_cast<Value*>(storage), std::forward<Arguments>(arguments)...);
    }

    void reset() noexcept {
        storage_.release();
    }

    [[nodiscard]] std::pmr::memory_resource& resource() noexcept {
        return storage_;
    }

private:
    std::pmr::monotonic_buffer_resource storage_;
};

/** One bounded transient epoch. All arena storage is reclaimed on both entry and exit. */
class ArenaScope final {
public:
    explicit ArenaScope(Arena& arena) noexcept : arena_(arena) { arena_.reset(); }
    ~ArenaScope() { arena_.reset(); }

    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;

private:
    Arena& arena_;
};

} // namespace strata::core
