#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "browser_model.hpp"
#include "data/json.hpp"
#include "scenario.hpp"

namespace strata::headless {

/** Persistent deterministic application session shared by batch scenarios and interactive control.
 */
class Session final {
  public:
    Session(const Scenario& scenario, std::filesystem::path resource_root,
            std::filesystem::path output_root);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void execute(const ScenarioStep& step);
    void ensure_frame();
    void write_current();
    void write_result() const;
    void close();

    [[nodiscard]] BrowserModel browser_model() const;
    [[nodiscard]] data::JsonValue interactive_state(std::string_view event) const;
    [[nodiscard]] std::int64_t time_nanoseconds() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::headless
