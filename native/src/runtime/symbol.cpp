#include "runtime/symbol.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace strata::runtime {
namespace {

constexpr std::uint32_t chunk_bits = 12U;
constexpr std::uint32_t chunk_size = 1U << chunk_bits;
constexpr std::uint32_t chunk_count = 1U << 12U;

/**
 * Names live in a deque, so views of them stay valid; an id's view sits in a fixed chunk whose
 * pointer is published once, so reading a name takes no lock.
 */
class SymbolTable final {
  public:
    SymbolTable() {
        static_cast<void>(insert_locked(std::string_view{}));
    }

    [[nodiscard]] std::uint32_t intern(const std::string_view name) {
        {
            std::shared_lock lock(mutex_);
            if (const auto found = ids_.find(name); found != ids_.end())
                return found->second;
        }
        std::unique_lock lock(mutex_);
        if (const auto found = ids_.find(name); found != ids_.end())
            return found->second;
        return insert_locked(name);
    }

    [[nodiscard]] std::optional<std::uint32_t> find(const std::string_view name) const {
        std::shared_lock lock(mutex_);
        const auto found = ids_.find(name);
        return found != ids_.end() ? std::optional<std::uint32_t>(found->second) : std::nullopt;
    }

    [[nodiscard]] std::string_view name(const std::uint32_t id) const noexcept {
        const std::string_view* chunk = chunks_[id >> chunk_bits].load(std::memory_order_acquire);
        return chunk != nullptr ? chunk[id & (chunk_size - 1U)] : std::string_view{};
    }

  private:
    [[nodiscard]] std::uint32_t insert_locked(const std::string_view name) {
        const std::uint32_t id = next_;
        if (id >> chunk_bits >= chunk_count)
            throw std::length_error("symbol table exhausted");
        std::atomic<const std::string_view*>& slot = chunks_[id >> chunk_bits];
        std::string_view* chunk =
            const_cast<std::string_view*>(slot.load(std::memory_order_relaxed));
        if (chunk == nullptr) {
            owned_.push_back(std::make_unique<std::string_view[]>(chunk_size));
            chunk = owned_.back().get();
        }
        const std::string& stored = names_.emplace_back(name);
        chunk[id & (chunk_size - 1U)] = stored;
        slot.store(chunk, std::memory_order_release);
        ids_.emplace(std::string_view(stored), id);
        ++next_;
        return id;
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string_view, std::uint32_t> ids_;
    std::deque<std::string> names_;
    std::deque<std::unique_ptr<std::string_view[]>> owned_;
    std::array<std::atomic<const std::string_view*>, chunk_count> chunks_{};
    std::uint32_t next_ = 0U;
};

[[nodiscard]] SymbolTable& table() {
    static SymbolTable instance;
    return instance;
}

} // namespace

Symbol Symbol::intern(const std::string_view name) {
    return Symbol(table().intern(name));
}

std::optional<Symbol> Symbol::find(const std::string_view name) {
    const std::optional<std::uint32_t> id = table().find(name);
    return id.has_value() ? std::optional<Symbol>(Symbol(*id)) : std::nullopt;
}

std::string_view Symbol::name() const noexcept {
    return table().name(id_);
}

} // namespace strata::runtime
