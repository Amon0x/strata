#include "durable_writer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace strata::desktop {
namespace {

/** One failed write attempt; transient failures are retried, the rest are reported at once. */
struct WriteFailure final {
    std::string message;
    bool transient = false;
};

[[nodiscard]] bool transient_error(const DWORD error) noexcept {
    // Another process (antivirus, indexer, backup) briefly holding the file.
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
           error == ERROR_ACCESS_DENIED;
}

[[nodiscard]] WriteFailure win32_failure(const std::string_view step, const DWORD error) {
    return WriteFailure{
        std::string(step) + " failed: " + std::system_category().message(static_cast<int>(error)),
        transient_error(error),
    };
}

/** Owns a Win32 file handle for the temporary file. */
class FileHandle final {
  public:
    explicit FileHandle(const HANDLE value) noexcept : value_(value) {}
    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    [[nodiscard]] bool close() noexcept {
        const bool closed = CloseHandle(value_) != FALSE;
        value_ = INVALID_HANDLE_VALUE;
        return closed;
    }

  private:
    HANDLE value_;
};

/**
 * Writes `payload` to a sibling temporary file, flushes it to disk, and atomically replaces
 * `path` with a write-through rename. Returns the failure, if any, of this one attempt.
 */
[[nodiscard]] std::optional<WriteFailure> write_once(const std::filesystem::path& path,
                                                     const std::string_view payload) {
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        return WriteFailure{"creating the durable directory failed: " + directory_error.message(),
                            false};
    }
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        FileHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
            return win32_failure("opening the temporary durable file", GetLastError());
        std::size_t written = 0U;
        while (written < payload.size()) {
            const DWORD chunk =
                static_cast<DWORD>(std::min<std::size_t>(payload.size() - written, 1U << 30U));
            DWORD wrote = 0U;
            if (!WriteFile(file.get(), payload.data() + written, chunk, &wrote, nullptr))
                return win32_failure("writing the temporary durable file", GetLastError());
            written += wrote;
        }
        // The data must be on disk before the rename can make it the durable copy.
        if (!FlushFileBuffers(file.get()))
            return win32_failure("flushing the temporary durable file", GetLastError());
        if (!file.close())
            return win32_failure("closing the temporary durable file", GetLastError());
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return win32_failure("replacing the durable file", error);
    }
    return std::nullopt;
}

} // namespace

DurableWriter::DurableWriter(FailureReport report) : DurableWriter(std::move(report), Options{}) {}

DurableWriter::DurableWriter(FailureReport report, const Options options)
    : report_(std::move(report)), options_(options), worker_([this] { run(); }) {}

DurableWriter::~DurableWriter() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
}

void DurableWriter::enqueue(std::filesystem::path path, std::string payload) {
    {
        const std::scoped_lock lock(mutex_);
        pending_.insert_or_assign(std::move(path), Pending{++version_, std::move(payload), false});
    }
    changed_.notify_one();
}

std::optional<std::string> DurableWriter::pending(const std::filesystem::path& path) const {
    const std::scoped_lock lock(mutex_);
    const auto found = pending_.find(path);
    return found != pending_.end() ? std::optional<std::string>(found->second.payload)
                                   : std::nullopt;
}

std::vector<std::filesystem::path> DurableWriter::flush() {
    std::unique_lock lock(mutex_);
    settled_.wait(lock, [this] { return !has_unwritten(); });
    std::vector<std::filesystem::path> parked;
    for (const auto& [path, entry] : pending_) {
        if (entry.parked)
            parked.push_back(path);
    }
    return parked;
}

bool DurableWriter::has_unwritten() const noexcept {
    return std::ranges::any_of(pending_, [](const auto& entry) { return !entry.second.parked; });
}

void DurableWriter::run() noexcept {
    for (;;) {
        std::filesystem::path path;
        Pending current;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || has_unwritten(); });
            if (!has_unwritten())
                return; // Stopping with nothing left to write; parked paths stay unwritten.
            const auto next = std::ranges::find_if(
                pending_, [](const auto& entry) { return !entry.second.parked; });
            path = next->first;
            current = next->second;
        }
        std::optional<WriteFailure> failure;
        try {
            std::chrono::milliseconds backoff = options_.initial_backoff;
            for (unsigned attempt = 1U;; ++attempt) {
                failure = write_once(path, current.payload);
                if (!failure.has_value() || !failure->transient || attempt >= options_.attempts)
                    break;
                std::this_thread::sleep_for(backoff);
                backoff *= 2;
            }
        } catch (const std::exception& error) {
            failure = WriteFailure{error.what(), false};
        } catch (...) {
            failure = WriteFailure{"the durable write failed", false};
        }
        // Report before settling, so a flush that returns has seen every report it waited for.
        if (failure.has_value() && report_) {
            try {
                report_(path, failure->message);
            } catch (...) {
                // A failing reporter must not stop the writer.
            }
        }
        {
            const std::scoped_lock lock(mutex_);
            const auto found = pending_.find(path);
            if (found != pending_.end() && found->second.version == current.version) {
                if (failure.has_value())
                    found->second.parked = true; // Keep serving it; the next payload retries.
                else
                    pending_.erase(found);
            }
            if (!has_unwritten())
                settled_.notify_all();
        }
    }
}

} // namespace strata::desktop
