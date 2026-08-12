#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace strata::desktop {

class ApplicationHost;

class PreviewSession final {
  public:
    PreviewSession(std::filesystem::path manifest_path, std::filesystem::path resource_root,
                   ApplicationHost& host, std::vector<std::string> immutable_resources,
                   std::vector<std::filesystem::path> extension_search_paths);
    ~PreviewSession();

    PreviewSession(const PreviewSession&) = delete;
    PreviewSession& operator=(const PreviewSession&) = delete;

    void poll() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
