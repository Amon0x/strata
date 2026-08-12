#pragma once

#include <strata/extension_plugin.h>
#include <strata/strata.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace strata::host {

/** One queried external package and the native library that owns all of its borrowed pointers. */
class LoadedExtension final {
  public:
    LoadedExtension() = default;
    LoadedExtension(const LoadedExtension&) = delete;
    LoadedExtension& operator=(const LoadedExtension&) = delete;
    LoadedExtension(LoadedExtension&& other) noexcept;
    LoadedExtension& operator=(LoadedExtension&& other) noexcept;
    ~LoadedExtension();

    [[nodiscard]] const std::string& id() const noexcept {
        return id_;
    }
    [[nodiscard]] const std::string& schema_json() const noexcept {
        return schema_json_;
    }
    [[nodiscard]] const strata_surface_extension_bundle& bundle() const noexcept {
        return *bundle_;
    }

  private:
    friend struct SelectedExtensions;
    friend LoadedExtension load_extension(std::string_view,
                                          const std::vector<std::filesystem::path>&);

    void close() noexcept;

    void* library_ = nullptr;
    std::filesystem::path path_;
    std::string id_;
    std::string schema_json_;
    const strata_surface_extension_bundle* bundle_ = nullptr;
};

struct SelectedExtensions final {
    /* Declared first so libraries are destroyed last, after every copied callback descriptor. */
    std::vector<LoadedExtension> packages;
    std::vector<strata_widget_extension> widgets;
    std::vector<strata_widget_input_extension> widget_inputs;
    std::vector<strata_widget_scroll_extension> widget_scrolls;
    std::vector<strata_behavior_input_extension> behavior_inputs;
    std::vector<strata_behavior_extension> behaviors;
    strata_surface_extension_bundle bundle{};

    [[nodiscard]] const strata_surface_extension_bundle* pointer() noexcept;
    [[nodiscard]] std::vector<std::string> schemas() const;
};

/** Reads the single extension package declaration from an application schema document. */
[[nodiscard]] std::vector<std::string> declared_extension_packages(std::string_view schemas_json);

/** Loads each selected package from a shared library and merges its runtime descriptor bundle. */
[[nodiscard]] SelectedExtensions
select_extensions(const std::vector<std::string>& package_ids,
                  const std::vector<std::filesystem::path>& search_directories = {});

} // namespace strata::host
