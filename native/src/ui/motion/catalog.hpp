#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "runtime/unit.hpp"
#include "ui/motion/model.hpp"

namespace strata::ui {

/** Lazily compiles immutable portable-IR animation declarations once per active runtime unit. */
class MotionCatalog final {
public:
    void bind(std::shared_ptr<const runtime::RuntimeUnit> unit);
    void set_supplemental(std::map<std::string, CompiledMotion, std::less<>> motions);
    [[nodiscard]] const CompiledMotion* find(std::string_view name);
    [[nodiscard]] const CompiledMotion* timed(
        std::string_view name,
        std::int64_t duration_nanos,
        std::int64_t delay_nanos
    );
    void clear() noexcept;

private:
    std::shared_ptr<const runtime::RuntimeUnit> unit_;
    std::map<std::string, CompiledMotion, std::less<>> compiled_;
    std::map<std::string, CompiledMotion, std::less<>> timed_;
    std::map<std::string, CompiledMotion, std::less<>> supplemental_;
};

} // namespace strata::ui
