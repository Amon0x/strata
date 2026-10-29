#include "platform_io.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace strata::desktop {
namespace {

using data::JsonValue;
using Microsoft::WRL::ComPtr;
using namespace platform_io;

/** The most a file read may bring back. */
constexpr std::uint64_t read_limit = 64ULL * 1024ULL * 1024ULL;
/** The most entries a listing gathers. */
constexpr std::size_t list_limit = 10'000U;
/** A change reported again this soon after the same one is the same change. */
constexpr auto repeat_window = std::chrono::milliseconds(100);

class FileError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string shown(const std::filesystem::path& path) {
    return utf8(path.native());
}

[[nodiscard]] std::string system_text(const DWORD code) {
    std::string text = std::system_category().message(static_cast<int>(code));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

[[noreturn]] void fail(const std::string_view what, const std::filesystem::path& path) {
    const DWORD code = GetLastError();
    throw FileError(std::string(what) + " " + shown(path) + ": " + system_text(code));
}

/** Closes a Win32 handle when it goes. */
class Handle final {
  public:
    explicit Handle(const HANDLE value) noexcept : value_(value) {}
    ~Handle() {
        if (valid())
            CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

  private:
    HANDLE value_;
};

[[nodiscard]] std::uint64_t file_time(const FILETIME time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
}

[[nodiscard]] std::uint64_t file_size(const DWORD high, const DWORD low) noexcept {
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}

/** What the file system says of `path`, if anything is there. */
[[nodiscard]] std::optional<WIN32_FILE_ATTRIBUTE_DATA>
attributes(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return std::nullopt;
    return data;
}

[[nodiscard]] JsonValue::Object failure(std::string error) {
    return {{"error", JsonValue(std::move(error))}, {"ok", JsonValue(false)}};
}

[[nodiscard]] JsonValue::Object read_file(const std::filesystem::path& path,
                                          const std::string_view as) {
    if (as != "text" && as != "json" && as != "base64")
        throw FileError("as is \"text\", \"json\" or \"base64\"");
    const Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid())
        fail("could not open", path);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information))
        fail("could not read", path);
    const std::uint64_t size = file_size(information.nFileSizeHigh, information.nFileSizeLow);
    if (size > read_limit)
        throw FileError(shown(path) + " is larger than 64 MB");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t done = 0U;
    while (done < bytes.size()) {
        DWORD read = 0U;
        if (!ReadFile(file.get(), bytes.data() + done, static_cast<DWORD>(bytes.size() - done),
                      &read, nullptr)) {
            fail("could not read", path);
        }
        if (read == 0U)
            break;
        done += read;
    }
    bytes.resize(done);
    JsonValue::Object answer{
        {"modified", JsonValue(unix_millis(file_time(information.ftLastWriteTime)))},
        {"ok", JsonValue(true)},
        {"path", JsonValue(shown(path))},
        {"size", JsonValue(static_cast<std::int64_t>(bytes.size()))},
    };
    if (as == "base64") {
        const auto* const data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        answer.emplace_back("base64", JsonValue(base64_encode({data, bytes.size()})));
        return answer;
    }
    // Text starts after a UTF-8 byte order mark.
    if (bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0U, 3U);
    if (as == "json") {
        try {
            answer.emplace_back("json", data::parse_json(bytes));
        } catch (const std::exception& error) {
            throw FileError(shown(path) + " is not JSON: " + error.what());
        }
        return answer;
    }
    answer.emplace_back("text", JsonValue(std::move(bytes)));
    return answer;
}

[[nodiscard]] JsonValue::Object write_file(const std::filesystem::path& path,
                                           const JsonValue& payload) {
    std::string bytes;
    if (const JsonValue* text = payload.find("text");
        text != nullptr && text->string() != nullptr) {
        bytes = *text->string();
    } else if (const JsonValue* json = payload.find("json"); json != nullptr) {
        bytes = data::encode_canonical_json(*json);
    } else if (const JsonValue* base64 = payload.find("base64");
               base64 != nullptr && base64->string() != nullptr) {
        std::optional<std::vector<std::uint8_t>> decoded = base64_decode(*base64->string());
        if (!decoded.has_value())
            throw FileError("base64 is not base64");
        bytes.assign(decoded->begin(), decoded->end());
    } else {
        throw FileError("file.write needs text, json or base64");
    }
    if (flag_field(payload, "createDirectories", false) && path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            throw FileError("could not create " + shown(path.parent_path()) + ": " +
                            error.message());
    }
    const bool append = flag_field(payload, "append", false);
    const Handle file(CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (!file.valid())
        fail("could not open", path);
    std::size_t done = 0U;
    while (done < bytes.size()) {
        DWORD written = 0U;
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1U << 30U));
        if (!WriteFile(file.get(), bytes.data() + done, chunk, &written, nullptr))
            fail("could not write", path);
        done += written;
    }
    return {{"ok", JsonValue(true)},
            {"path", JsonValue(shown(path))},
            {"size", JsonValue(static_cast<std::int64_t>(done))}};
}

[[nodiscard]] JsonValue::Object file_info(const std::filesystem::path& path) {
    const std::optional<WIN32_FILE_ATTRIBUTE_DATA> found = attributes(path);
    if (!found.has_value()) {
        return {{"directory", JsonValue(false)},
                {"exists", JsonValue(false)},
                {"modified", JsonValue(0.0)},
                {"path", JsonValue(shown(path))},
                {"size", JsonValue(std::int64_t{0})}};
    }
    const bool directory = (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    return {{"directory", JsonValue(directory)},
            {"exists", JsonValue(true)},
            {"modified", JsonValue(unix_millis(file_time(found->ftLastWriteTime)))},
            {"path", JsonValue(shown(path))},
            {"size", JsonValue(directory ? std::int64_t{0}
                                         : static_cast<std::int64_t>(file_size(
                                               found->nFileSizeHigh, found->nFileSizeLow)))}};
}

/** Gathers `folder`'s entries whose names match `pattern`, descending when `recursive`. */
void gather(const std::filesystem::path& folder, const std::wstring& pattern, const bool recursive,
            JsonValue::Array& entries, bool& truncated) {
    WIN32_FIND_DATAW found{};
    const std::filesystem::path query = folder / L"*";
    const HANDLE search =
        FindFirstFileExW(query.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr,
                         FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::filesystem::path> below;
    do {
        const std::wstring_view name = found.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const bool directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
        const std::filesystem::path path = folder / found.cFileName;
        // Links to folders are listed, not followed: they may lead back up.
        if (recursive && directory && (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U)
            below.push_back(path);
        if (!pattern.empty() && !glob_match(pattern, name))
            continue;
        if (entries.size() >= list_limit) {
            truncated = true;
            break;
        }
        entries.emplace_back(JsonValue::Object{
            {"directory", JsonValue(directory)},
            {"modified", JsonValue(unix_millis(file_time(found.ftLastWriteTime)))},
            {"name", JsonValue(utf8(name))},
            {"path", JsonValue(shown(path))},
            {"size", JsonValue(directory ? std::int64_t{0}
                                         : static_cast<std::int64_t>(file_size(
                                               found.nFileSizeHigh, found.nFileSizeLow)))},
        });
    } while (FindNextFileW(search, &found));
    FindClose(search);
    for (const std::filesystem::path& folder_below : below) {
        if (truncated)
            return;
        gather(folder_below, pattern, recursive, entries, truncated);
    }
}

[[nodiscard]] JsonValue::Object list_folder(const std::filesystem::path& path,
                                            const JsonValue& payload) {
    const std::optional<WIN32_FILE_ATTRIBUTE_DATA> found = attributes(path);
    if (!found.has_value() || (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
        throw FileError("no folder at " + shown(path));
    JsonValue::Array entries;
    bool truncated = false;
    gather(path, wide(text_field(payload, "pattern")), flag_field(payload, "recursive", false),
           entries, truncated);
    // Folders first, then by path, as a file manager shows them.
    std::ranges::sort(entries, [](const JsonValue& left, const JsonValue& right) {
        const bool left_folder = *left.find("directory")->boolean();
        const bool right_folder = *right.find("directory")->boolean();
        if (left_folder != right_folder)
            return left_folder;
        return _stricmp(left.find("path")->string()->c_str(),
                        right.find("path")->string()->c_str()) < 0;
    });
    return {{"entries", JsonValue(std::move(entries))},
            {"ok", JsonValue(true)},
            {"path", JsonValue(shown(path))},
            {"truncated", JsonValue(truncated)}};
}

/** Moves `path` to the recycle bin; the shell warns before anything would be destroyed instead. */
[[nodiscard]] JsonValue::Object trash(const std::filesystem::path& path) {
    if (!attributes(path).has_value())
        throw FileError("nothing at " + shown(path));
    ComPtr<IFileOperation> operation;
    ComPtr<IShellItem> item;
    if (FAILED(
            CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation))) ||
        FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        throw FileError("the shell could not reach " + shown(path));
    }
    // FOF_WANTNUKEWARNING asks before a file too large for the bin would be deleted for good.
    operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI |
                                 FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE);
    BOOL aborted = FALSE;
    if (FAILED(operation->DeleteItem(item.Get(), nullptr)) ||
        FAILED(operation->PerformOperations()) ||
        FAILED(operation->GetAnyOperationsAborted(&aborted)) || aborted != FALSE ||
        attributes(path).has_value()) {
        throw FileError("could not move " + shown(path) + " to the recycle bin");
    }
    return {{"ok", JsonValue(true)}, {"path", JsonValue(shown(path))}};
}

/** A folder (or the one file in it) watched for changes on a thread of its own. */
class Watch final {
  public:
    Watch(PlatformOutbox& outbox, PlatformRequest request, std::string channel,
          std::filesystem::path folder, std::wstring only, const bool recursive)
        : outbox_(outbox), request_(std::move(request)), channel_(std::move(channel)),
          folder_(std::move(folder)), only_(std::move(only)), recursive_(recursive),
          directory_(CreateFileW(folder_.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                 nullptr)),
          stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          changed_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), buffer_(16U * 1024U) {
        if (!directory_.valid())
            fail("could not watch", folder_);
        // The first read starts the system recording changes: armed here, before file.watch
        // answers, a change made the moment it does is not missed.
        if (!stop_.valid() || !changed_.valid() || !arm())
            fail("could not watch", folder_);
        try {
            thread_ = std::jthread([this] { watch(); });
        } catch (...) {
            // The read in flight writes into this watch: it ends before the watch does.
            CancelIoEx(directory_.get(), &overlapped_);
            DWORD ignored = 0U;
            GetOverlappedResult(directory_.get(), &overlapped_, &ignored, TRUE);
            throw;
        }
    }
    ~Watch() {
        SetEvent(stop_.get());
        if (thread_.joinable())
            thread_.join();
    }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

  private:
    void say(const std::string_view type, const std::filesystem::path& path,
             const std::optional<std::filesystem::path>& from = std::nullopt) {
        const std::string where = shown(path);
        const auto now = std::chrono::steady_clock::now();
        // One save often reports a change twice (size, then time): say it once.
        if (type == last_type_ && where == last_path_ && now - last_said_ < repeat_window)
            return;
        last_type_ = type;
        last_path_ = where;
        last_said_ = now;
        JsonValue::Object data{{"path", JsonValue(where)}, {"type", JsonValue(std::string(type))}};
        if (from.has_value())
            data.emplace_back("oldPath", JsonValue(shown(*from)));
        outbox_.say(request_, channel_, JsonValue(std::move(data)));
    }

    [[nodiscard]] bool wanted(const std::wstring_view name) const {
        return only_.empty() ||
               CompareStringOrdinal(name.data(), static_cast<int>(name.size()), only_.data(),
                                    static_cast<int>(only_.size()), TRUE) == CSTR_EQUAL;
    }

    /** Asks for the next changes; the system records any made until the read is done. */
    [[nodiscard]] bool arm() {
        overlapped_ = OVERLAPPED{};
        overlapped_.hEvent = changed_.get();
        ResetEvent(changed_.get());
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                             FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
        return ReadDirectoryChangesW(directory_.get(), buffer_.data(),
                                     static_cast<DWORD>(buffer_.size() * sizeof(DWORD)),
                                     recursive_ ? TRUE : FALSE, filter, nullptr, &overlapped_,
                                     nullptr) != FALSE;
    }

    void watch() {
        std::optional<std::filesystem::path> renamed_from;
        for (;;) {
            const HANDLE waits[2] = {changed_.get(), stop_.get()};
            if (WaitForMultipleObjects(2U, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(directory_.get(), &overlapped_);
                DWORD ignored = 0U;
                GetOverlappedResult(directory_.get(), &overlapped_, &ignored, TRUE);
                return;
            }
            DWORD bytes = 0U;
            if (!GetOverlappedResult(directory_.get(), &overlapped_, &bytes, FALSE)) {
                // The folder itself went away.
                say("removed", folder_);
                return;
            }
            // What arrived is copied out, so the next read can start at once.
            std::vector<DWORD> arrived(
                buffer_.begin(),
                buffer_.begin() +
                    static_cast<std::ptrdiff_t>((bytes + sizeof(DWORD) - 1U) / sizeof(DWORD)));
            if (!arm()) {
                say("removed", folder_);
                return;
            }
            if (bytes == 0U) {
                // More changed than the buffer held: something here changed.
                say("changed", only_.empty() ? folder_ : folder_ / only_);
                continue;
            }
            const auto* cursor = reinterpret_cast<const std::uint8_t*>(arrived.data());
            for (;;) {
                const auto* const note = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
                const std::wstring_view name(note->FileName,
                                             note->FileNameLength / sizeof(wchar_t));
                const std::wstring_view leaf = name.substr(name.find_last_of(L'\\') + 1U);
                const std::filesystem::path path = folder_ / name;
                switch (note->Action) {
                case FILE_ACTION_ADDED:
                    if (wanted(leaf))
                        say("added", path);
                    break;
                case FILE_ACTION_REMOVED:
                    if (wanted(leaf))
                        say("removed", path);
                    break;
                case FILE_ACTION_MODIFIED:
                    if (wanted(leaf))
                        say("changed", path);
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    renamed_from = path;
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    if (wanted(leaf) ||
                        (renamed_from.has_value() && wanted(renamed_from->filename().native()))) {
                        say("renamed", path, renamed_from);
                    }
                    renamed_from.reset();
                    break;
                default:
                    break;
                }
                if (note->NextEntryOffset == 0U)
                    break;
                cursor += note->NextEntryOffset;
            }
        }
    }

    PlatformOutbox& outbox_;
    const PlatformRequest request_;
    const std::string channel_;
    const std::filesystem::path folder_;
    /** The one file watched in `folder_`; empty for the whole folder. */
    const std::wstring only_;
    const bool recursive_;
    Handle directory_;
    Handle stop_;
    Handle changed_;
    OVERLAPPED overlapped_{};
    /** DWORDs keep the notifications aligned as ReadDirectoryChangesW needs. */
    std::vector<DWORD> buffer_;
    std::string_view last_type_;
    std::string last_path_;
    std::chrono::steady_clock::time_point last_said_;
    std::jthread thread_;
};

class FileService final : public PlatformService {
  public:
    FileService(PlatformOutbox& outbox, ResourcePathResolver resolve)
        : outbox_(outbox), resolve_(std::move(resolve)),
          // One worker: a read queued after a write reads what it wrote.
          workers_(std::make_unique<PlatformWorkers>(1U)) {}

    ~FileService() override {
        watches_.clear();
        workers_.reset();
    }

    FileService(const FileService&) = delete;
    FileService& operator=(const FileService&) = delete;

    [[nodiscard]] bool serves(const std::string_view kind) const noexcept override {
        return kind.starts_with("file.");
    }

    void run(const PlatformRequest& request) override {
        if (request.kind == "file.unwatch") {
            const auto found = watches_.find(
                std::make_pair(request.client, text_field(request.payload, "channel")));
            const bool watching = found != watches_.end();
            if (watching)
                watches_.erase(found);
            outbox_.reply(request, {{"ok", JsonValue(watching)}});
            return;
        }
        const std::string named = requested_path(request.payload);
        if (named.empty()) {
            outbox_.reply(request, failure(request.kind + " needs a path"));
            return;
        }
        const std::filesystem::path path = resolve_path(named, resolve_);
        if (request.kind == "file.watch") {
            outbox_.reply(request, watch(request, path));
            return;
        }
        workers_->submit([this, request, path] {
            JsonValue::Object answer;
            try {
                answer = perform(request, path);
            } catch (const std::exception& error) {
                answer = failure(error.what());
                answer.emplace_back("path", JsonValue(shown(path)));
            }
            outbox_.reply(request, std::move(answer));
        });
    }

    void close_client(const std::string_view client) override {
        std::erase_if(watches_,
                      [client](const auto& entry) { return entry.first.first == client; });
    }

  private:
    [[nodiscard]] static JsonValue::Object perform(const PlatformRequest& request,
                                                   const std::filesystem::path& path) {
        if (request.kind == "file.read")
            return read_file(path, text_field(request.payload, "as", "text"));
        if (request.kind == "file.write")
            return write_file(path, request.payload);
        if (request.kind == "file.list")
            return list_folder(path, request.payload);
        if (request.kind == "file.info")
            return file_info(path);
        if (request.kind == "file.trash")
            return trash(path);
        return failure("unknown platform request " + request.kind);
    }

    [[nodiscard]] JsonValue::Object watch(const PlatformRequest& request,
                                          const std::filesystem::path& path) {
        const std::string channel = text_field(request.payload, "channel");
        if (channel.empty())
            return failure("file.watch needs a channel");
        const std::optional<WIN32_FILE_ATTRIBUTE_DATA> found = attributes(path);
        if (!found.has_value())
            return failure("nothing at " + shown(path));
        const bool folder = (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
        const auto key = std::make_pair(request.client, channel);
        // A channel watches one thing: watching again replaces what it watched.
        watches_.erase(key);
        try {
            watches_.emplace(key, std::make_unique<Watch>(
                                      outbox_, request, channel, folder ? path : path.parent_path(),
                                      folder ? std::wstring{} : path.filename().native(),
                                      folder && flag_field(request.payload, "recursive", false)));
        } catch (const std::exception& error) {
            return failure(error.what());
        }
        return {{"ok", JsonValue(true)}, {"path", JsonValue(shown(path))}};
    }

    PlatformOutbox& outbox_;
    ResourcePathResolver resolve_;
    std::map<std::pair<std::string, std::string>, std::unique_ptr<Watch>> watches_;
    std::unique_ptr<PlatformWorkers> workers_;
};

} // namespace

std::unique_ptr<PlatformService> make_file_service(PlatformOutbox& outbox,
                                                   ResourcePathResolver resolve) {
    return std::make_unique<FileService>(outbox, std::move(resolve));
}

} // namespace strata::desktop
