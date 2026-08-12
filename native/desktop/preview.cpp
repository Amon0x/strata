#include "preview.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <strata/desktop.hpp>

namespace strata::desktop {
namespace {

using Clock = std::chrono::steady_clock;

struct FileSignature final {
    std::filesystem::file_time_type modified{};
    std::uintmax_t size = 0U;
    bool exists = false;
    [[nodiscard]] friend bool operator==(const FileSignature&, const FileSignature&) = default;
};

[[nodiscard]] FileSignature signature(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(path, error);
    if (error || !exists)
        return {};
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, error);
    if (error)
        return {};
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
        return {};
    return FileSignature{modified, size, true};
}

[[nodiscard]] std::filesystem::path normalized(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

} // namespace

struct PreviewSession::Impl final {
    enum class Kind { module, program, restart };

    struct WatchedFile final {
        std::filesystem::path path;
        std::string resource_id;
        FileSignature observed;
        Kind kind = Kind::restart;
    };

    Impl(std::filesystem::path manifest_path, std::filesystem::path resource_root,
         ApplicationHost& host, std::vector<std::string> immutable_resources,
         std::vector<std::filesystem::path> extension_search_paths)
        : resource_root(normalized(resource_root)), host(host) {
        add_path(std::move(manifest_path), {}, Kind::restart);
        for (const std::string& resource : immutable_resources) {
            add_resource(resource, Kind::restart);
        }
        for (const std::filesystem::path& directory : extension_search_paths) {
            std::error_code error;
            for (std::filesystem::directory_iterator current(directory, error), end;
                 !error && current != end; current.increment(error)) {
                if (current->path().extension() == ".dll") {
                    add_path(current->path(), {}, Kind::restart);
                }
            }
        }
        refresh_source_watches();
        const auto count = [this](const Kind kind) {
            std::size_t result = 0U;
            for (const auto& [path, watched] : files) {
                static_cast<void>(path);
                if (watched.kind == kind)
                    ++result;
            }
            return result;
        };
        std::cout << "STRATA PREVIEW READY modules=" << count(Kind::module)
                  << " hlsl=" << count(Kind::program) << " immutable=" << count(Kind::restart)
                  << '\n'
                  << std::flush;
    }

    void add_resource(const std::string_view resource_id, const Kind kind) {
        if (resource_id.empty())
            return;
        add_path(resource_root / std::filesystem::path(resource_id), resource_id, kind);
    }

    void add_path(std::filesystem::path path, const std::string_view resource_id, const Kind kind) {
        path = normalized(path);
        const auto found = files.find(path);
        if (found != files.end()) {
            if (found->second.kind == Kind::restart && kind != Kind::restart) {
                found->second.kind = kind;
                found->second.resource_id = resource_id;
            }
            return;
        }
        files.emplace(path, WatchedFile{path, std::string(resource_id), signature(path), kind});
    }

    void refresh_source_watches() {
        for (const std::string& resource : host.module_resources()) {
            add_resource(resource, Kind::module);
        }
        for (const std::string& resource : host.program_resources()) {
            add_resource(resource, Kind::program);
        }
    }

    void scan(const Clock::time_point now) {
        for (auto& [path, watched] : files) {
            static_cast<void>(path);
            const FileSignature current = signature(watched.path);
            if (current == watched.observed)
                continue;
            watched.observed = current;
            apply_after = now + std::chrono::milliseconds(120);
            pending = true;
            if (watched.kind == Kind::module) {
                source_pending = true;
            } else if (watched.kind == Kind::program) {
                program_pending.insert(watched.resource_id);
            } else {
                restart_pending.insert(watched.path);
            }
        }
    }

    void print_diagnostics(const std::size_t start) const {
        const std::vector<Diagnostic>& values = host.diagnostics();
        for (std::size_t index = start; index < values.size(); ++index) {
            const Diagnostic& diagnostic = values[index];
            std::cerr << "STRATA PREVIEW DIAGNOSTIC " << diagnostic.code << ' '
                      << diagnostic.source;
            if (diagnostic.line != 0U) {
                std::cerr << ':' << diagnostic.line << ':' << diagnostic.column;
            }
            std::cerr << ' ' << diagnostic.message << '\n';
        }
    }

    void apply() {
        pending = false;
        if (source_pending) {
            source_pending = false;
            const std::size_t diagnostic_start = host.diagnostics().size();
            try {
                if (host.reactivate()) {
                    std::cout << "STRATA PREVIEW SOURCE APPLIED\n" << std::flush;
                } else {
                    std::cerr << "STRATA PREVIEW SOURCE REJECTED; last-good UI retained\n";
                }
                print_diagnostics(diagnostic_start);
            } catch (const std::exception& error) {
                std::cerr << "STRATA PREVIEW SOURCE REJECTED; last-good UI retained: "
                          << error.what() << '\n';
            }
            refresh_source_watches();
        }

        std::set<std::string, std::less<>> programs;
        programs.swap(program_pending);
        for (const std::string& resource_id : programs) {
            try {
                if (host.reload_program_source(resource_id)) {
                    std::cout << "STRATA PREVIEW HLSL APPLIED " << resource_id << '\n'
                              << std::flush;
                } else {
                    std::cerr << "STRATA PREVIEW RESTART REQUIRED " << resource_id
                              << ": shader is no longer predeclared\n";
                }
            } catch (const std::exception& error) {
                std::cerr << "STRATA PREVIEW HLSL REJECTED; last-good program retained "
                          << resource_id << ": " << error.what() << '\n';
            }
        }

        std::set<std::filesystem::path> restart;
        restart.swap(restart_pending);
        for (const std::filesystem::path& path : restart) {
            std::cerr << "STRATA PREVIEW RESTART REQUIRED " << path.string()
                      << ": manifest, schema, extension, font, and image inputs are immutable\n";
        }
    }

    void poll() noexcept {
        try {
            const Clock::time_point now = Clock::now();
            if (now >= next_scan) {
                next_scan = now + std::chrono::milliseconds(50);
                scan(now);
            }
            if (pending && now >= apply_after)
                apply();
        } catch (const std::exception& error) {
            std::cerr << "STRATA PREVIEW WATCH ERROR " << error.what() << '\n';
        }
    }

    std::filesystem::path resource_root;
    ApplicationHost& host;
    std::map<std::filesystem::path, WatchedFile> files;
    std::set<std::string, std::less<>> program_pending;
    std::set<std::filesystem::path> restart_pending;
    Clock::time_point next_scan = Clock::now();
    Clock::time_point apply_after{};
    bool source_pending = false;
    bool pending = false;
};

PreviewSession::PreviewSession(std::filesystem::path manifest_path,
                               std::filesystem::path resource_root, ApplicationHost& host,
                               std::vector<std::string> immutable_resources,
                               std::vector<std::filesystem::path> extension_search_paths)
    : impl_(std::make_unique<Impl>(std::move(manifest_path), std::move(resource_root), host,
                                   std::move(immutable_resources),
                                   std::move(extension_search_paths))) {}

PreviewSession::~PreviewSession() = default;

void PreviewSession::poll() noexcept {
    impl_->poll();
}

} // namespace strata::desktop
