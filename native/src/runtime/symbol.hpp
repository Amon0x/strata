#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace strata::runtime {

/**
 * An interned name. Every distinct name keeps one id for the life of the process, so names compare
 * and hash as integers and a symbol from one program means the same name in any other. The
 * default symbol is the empty name.
 */
class Symbol final {
  public:
    constexpr Symbol() noexcept = default;

    [[nodiscard]] static Symbol intern(std::string_view name);
    /** The symbol of a name already interned, without interning it: none for a name nothing
     * declared, which therefore cannot be bound anywhere. */
    [[nodiscard]] static std::optional<Symbol> find(std::string_view name);

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] constexpr std::uint32_t id() const noexcept {
        return id_;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return id_ == 0U;
    }

    [[nodiscard]] friend constexpr bool operator==(Symbol, Symbol) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(Symbol, Symbol) noexcept = default;

  private:
    constexpr explicit Symbol(const std::uint32_t id) noexcept : id_(id) {}

    std::uint32_t id_ = 0U;
};

} // namespace strata::runtime

template <> struct std::hash<strata::runtime::Symbol> {
    [[nodiscard]] std::size_t operator()(const strata::runtime::Symbol symbol) const noexcept {
        return std::hash<std::uint32_t>{}(symbol.id());
    }
};
