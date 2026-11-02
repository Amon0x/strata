#include "core/runtime.hpp"

#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "base/utf8.hpp"
#include "compiler/artifact.hpp"

namespace strata::core {
namespace {

struct InvalidHostRoots final : std::invalid_argument {
    InvalidHostRoots() : std::invalid_argument("Host snapshot values must be one JSON object.") {}
};

/** Only ordered publications defer reporting; other snapshot-kind tasks use the normal reporter. */
struct HostPublicationFailure final {
    std::exception_ptr cause;
};

/** Worker inputs retain the schema which applied at admission, not a later configuration. */
[[nodiscard]] std::shared_ptr<const runtime::HostSnapshot> prepare_host_snapshot(
    std::string id, const std::uint64_t generation, const std::string& json,
    const std::shared_ptr<const runtime::ApplicationBundle>& bundle) {
    const data::JsonValue values = data::parse_json(json);
    if (values.object() == nullptr) throw InvalidHostRoots{};
    return bundle != nullptr
        ? bundle->host_snapshot(std::move(id), generation, values)
        : runtime::HostSnapshot::from_json(std::move(id), generation, values);
}

} // namespace

HostPublication::HostPublication(std::string id, const std::uint64_t generation)
    : id_(std::move(id)), generation_(generation) {}

void HostPublication::finish(const HostPublicationStatus status, const base::Result result,
                             std::string message) {
    result_ = result;
    message_ = std::move(message);
    status_.store(status, std::memory_order_release);
}

Runtime::Runtime(
    const base::HostAllocator allocator,
    const Clock clock,
    DiagnosticSink diagnostics,
    const std::uint64_t stable_identity_seed,
    const base::TaskExecution execution,
    base::TaskOwner::Wake wake
)
    : allocator_(allocator),
      memory_resource_(allocator),
      scratch_arena_(memory_resource_),
      diagnostics_(std::move(diagnostics)),
      tasks_(execution, std::move(wake), nullptr,
             [this](const base::TaskKind kind, const std::exception_ptr& failure) {
                 std::string message = std::string(base::task_kind_name(kind)) +
                                       " preparation failed";
                 try {
                     std::rethrow_exception(failure);
                 } catch (const HostPublicationFailure&) {
                     return;
                 } catch (const std::exception& error) {
                     if (base::valid_utf8(error.what()))
                         message += ": " + std::string(error.what());
                 } catch (...) {
                 }
                 constexpr std::string_view code = "STRATA.TASK.PREPARATION_FAILED";
                 if (application_ != nullptr) {
                     application_->services().report(
                         runtime::RuntimeDiagnostic{std::string(code), std::move(message), {},
                                                    std::nullopt});
                 } else {
                     static_cast<void>(diagnostics_.emit(base::Status::internal_error,
                                                        DiagnosticSeverity::error, code, message));
                 }
             }),
      clock_(clock),
      identities_(stable_identity_seed) {}

Runtime::~Runtime() {
    tasks_.close();
    for (PendingHostPublication& pending : host_publications_)
        pending.receipt->finish(HostPublicationStatus::cancelled);
    host_publications_.clear();
    published_application_.store(nullptr, std::memory_order_release);
    application_.reset();
}

base::HostAllocator Runtime::allocator() const noexcept {
    return allocator_;
}

base::AllocatorStatistics Runtime::allocator_statistics() const noexcept {
    return allocator_.statistics();
}

Diagnostics& Runtime::diagnostics() noexcept {
    return diagnostics_;
}

base::Result Runtime::next_identity(std::uint64_t& identity) noexcept {
    const auto next = identities_.next();
    if (!next.has_value()) {
        return diagnostics_.emit(
            base::Status::invariant_failure,
            DiagnosticSeverity::fatal,
            "STRATA.CORE.IDENTITY_EXHAUSTED",
            "The runtime-local stable identity space is exhausted."
        );
    }
    identity = *next;
    return base::result(base::Status::ok);
}

base::Result Runtime::create_snapshot(SnapshotData& snapshot) noexcept {
    std::int64_t now = 0;
    try {
        now = clock_.now_nanoseconds(clock_.user_data);
    } catch (...) {
        return diagnostics_.emit(
            base::Status::internal_error,
            DiagnosticSeverity::fatal,
            "STRATA.ABI.CLOCK_CALLBACK_FAILED",
            "The caller-owned clock callback raised an exception."
        );
    }
    if (has_sampled_time_ && now < last_time_nanoseconds_) {
        return diagnostics_.emit(
            base::Status::clock_regression,
            DiagnosticSeverity::error,
            "STRATA.RUNTIME.CLOCK_REGRESSION",
            "The caller-owned monotonic clock moved backwards."
        );
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return diagnostics_.emit(
            base::Status::invariant_failure,
            DiagnosticSeverity::fatal,
            "STRATA.CORE.GENERATION_EXHAUSTED",
            "The runtime snapshot generation is exhausted."
        );
    }
    last_time_nanoseconds_ = now;
    has_sampled_time_ = true;
    ++generation_;
    snapshot = SnapshotData{generation_, now, identities_.last()};
    return base::result(base::Status::ok);
}

std::shared_ptr<HostPublication> Runtime::publish_host_snapshot(
    std::string id,
    const std::uint64_t generation,
    std::string values_json,
    const bool inline_execution
) {
    if (id.empty() || !base::valid_utf8(id))
        throw std::invalid_argument("host snapshot id must be non-empty valid UTF-8");
    const bool immediate = inline_execution ||
        tasks_.execution() == base::TaskExecution::deterministic;
    if (immediate && !host_publications_.empty())
        throw std::invalid_argument("inline snapshot publication requires no pending publications");
    auto receipt = std::make_shared<HostPublication>(std::move(id), generation);
    const auto [accepted, inserted] = accepted_host_generations_.try_emplace(receipt->id(), generation);
    if (!inserted && generation <= accepted->second)
        throw std::invalid_argument(
            "host snapshot generations must increase strictly within one snapshot id");
    if (immediate) {
        accepted->second = generation;
        try {
            static_cast<void>(host_store_.adopt(
                prepare_host_snapshot(receipt->id(), generation, values_json, application_bundle_),
                application_ != nullptr ? &application_->host() : nullptr));
            receipt->finish(HostPublicationStatus::adopted);
        } catch (...) {
            fail_host_snapshot(*receipt, std::current_exception());
        }
        return receipt;
    }
    try {
        host_publications_.push_back(PendingHostPublication{
            receipt, {}, application_ != nullptr});
        try {
            host_publications_.back().prepared =
                base::Pending<std::shared_ptr<const runtime::HostSnapshot>>::start(
                    tasks_, base::TaskKind::snapshot, base::TaskPriority::frame_critical,
                    receipt->id(),
                    [producer = receipt->id(), generation, json = std::move(values_json),
                     bundle = application_bundle_](const base::TaskCancellation&) mutable {
                        try {
                            return prepare_host_snapshot(std::move(producer), generation, json, bundle);
                        } catch (...) {
                            throw HostPublicationFailure{std::current_exception()};
                        }
                    });
        } catch (...) {
            host_publications_.pop_back();
            throw;
        }
    } catch (...) {
        if (inserted) accepted_host_generations_.erase(accepted);
        throw;
    }
    accepted->second = generation;
    return receipt;
}

void Runtime::fail_host_snapshot(HostPublication& receipt, const std::exception_ptr& failure) {
    base::Status status = base::Status::internal_error;
    std::string_view code = "STRATA.HOST.INVALID_SNAPSHOT";
    std::string message;
    try {
        std::rethrow_exception(failure);
    } catch (const HostPublicationFailure& error) {
        fail_host_snapshot(receipt, error.cause);
        return;
    } catch (const InvalidHostRoots& error) {
        status = base::Status::invalid_argument;
        code = "STRATA.HOST.INVALID_ROOTS";
        message = error.what();
    } catch (const data::JsonError& error) {
        status = base::Status::invalid_argument;
        code = "STRATA.HOST.INVALID_JSON";
        message = error.what();
    } catch (const std::bad_alloc&) {
        status = base::Status::out_of_memory;
        message = "Host snapshot preparation ran out of memory.";
    } catch (const std::invalid_argument& error) {
        status = base::Status::invalid_argument;
        message = error.what();
    } catch (const std::exception& error) {
        message = error.what();
    } catch (...) {
        message = "Host snapshot preparation raised an unknown exception.";
    }
    const base::Result result = diagnostics_.emit(status, DiagnosticSeverity::error, code, message);
    receipt.finish(HostPublicationStatus::failed, result, std::move(message));
}

void Runtime::adopt_host_snapshots() {
    while (!host_publications_.empty()) {
        PendingHostPublication& pending = host_publications_.front();
        if (pending.receipt->cancel_requested_.load(std::memory_order_acquire)) {
            pending.prepared.cancel();
            pending.receipt->finish(HostPublicationStatus::cancelled);
        } else if (pending.prepared.status() == base::PendingStatus::pending) {
            break;
        } else if (pending.prepared.status() == base::PendingStatus::failed) {
            const base::TaskOutcome failure = pending.prepared.readiness().failure();
            if (failure.cancelled)
                pending.receipt->finish(HostPublicationStatus::cancelled);
            else
                fail_host_snapshot(*pending.receipt, failure.error);
        } else {
            try {
                static_cast<void>(host_store_.adopt(
                    *pending.prepared.get(), pending.application_target ? &application_->host() : nullptr));
                pending.receipt->finish(HostPublicationStatus::adopted);
            } catch (...) {
                fail_host_snapshot(*pending.receipt, std::current_exception());
            }
        }
        host_publications_.pop_front();
    }
}

void Runtime::poll_host_snapshots() {
    static_cast<void>(tasks_.drain());
    adopt_host_snapshots();
}

std::optional<std::uint64_t> Runtime::accepted_host_snapshot_generation(
    const std::string_view id) const noexcept {
    const auto found = accepted_host_generations_.find(id);
    return found != accepted_host_generations_.end()
        ? std::optional<std::uint64_t>(found->second) : std::nullopt;
}

const std::shared_ptr<const runtime::HostSnapshot>& Runtime::host_snapshot() const noexcept {
    return host_store_.snapshot();
}

std::optional<std::uint64_t> Runtime::host_snapshot_generation(
    const std::string_view id
) const noexcept {
    return host_store_.generation(id);
}

std::optional<runtime::Value> Runtime::read_host_value(const std::string_view path) const {
    return host_store_.resolve(path);
}

void Runtime::configure_application(
    std::string id,
    const data::JsonValue* const schemas,
    const std::span<const data::JsonValue> extension_declarations
) {
    if (application_ != nullptr) {
        throw std::logic_error("runtime application is already configured");
    }
    std::shared_ptr<const runtime::ApplicationBundle> bundle =
        runtime::ApplicationBundle::create(schemas, {}, extension_declarations);
    auto application = std::make_unique<runtime::ApplicationContext>(
        std::move(id),
        bundle,
        [this](
            const runtime::RuntimeDiagnosticRecord& diagnostic,
            const std::uint64_t dropped_count
        ) {
            static_cast<void>(diagnostics_.publish(diagnostic, dropped_count));
        },
        &tasks_,
        [this] { adopt_host_snapshots(); }
    );
    application_bundle_ = std::move(bundle);
    application_ = std::move(application);
    published_application_.store(application_.get(), std::memory_order_release);
}

bool Runtime::has_application() const noexcept {
    return published_application_.load(std::memory_order_acquire) != nullptr;
}

runtime::ApplicationContext& Runtime::application() {
    auto* application = published_application_.load(std::memory_order_acquire);
    if (application == nullptr) throw std::logic_error("runtime application is not configured");
    return *application;
}

const runtime::ApplicationContext& Runtime::application() const {
    const auto* application = published_application_.load(std::memory_order_acquire);
    if (application == nullptr) throw std::logic_error("runtime application is not configured");
    return *application;
}

runtime::ActivationResult Runtime::compile_and_activate(
    const compiler::ModuleSource& entry,
    const compiler::ModuleLoader& loader,
    const std::uint64_t generation
) {
    auto profiler_frame = application().profiler().frame(generation);
    base::ArenaScope scratch_scope(scratch_arena_);
    return application().compile_and_activate(
        entry,
        loader,
        generation,
        &scratch_arena_.resource()
    );
}

runtime::ActivationResult Runtime::activate_compiled(
    const std::span<const std::uint8_t> artifact,
    const std::uint64_t generation
) {
    auto profiler_frame = application().profiler().frame(generation);
    compiler::CompiledModuleArtifact decoded;
    {
        auto decode = application().profiler().section("artifact-decode");
        decoded = compiler::decode_compiled_module_artifact(artifact);
    }
    return application().activate(
        std::move(decoded.unit),
        generation,
        std::move(decoded.source_map)
    );
}

} // namespace strata::core
