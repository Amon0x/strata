#include "core/runtime.hpp"

#include "compiler/artifact.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::core {
Runtime::Runtime(
    const HostAllocator allocator,
    const strata_clock clock,
    const strata_diagnostic_sink diagnostics,
    const std::uint64_t stable_identity_seed
)
    : allocator_(allocator),
      memory_resource_(allocator),
      scratch_arena_(memory_resource_),
      diagnostics_(diagnostics),
      clock_(clock),
      identities_(stable_identity_seed) {}

HostAllocator Runtime::allocator() const noexcept {
    return allocator_;
}

AllocatorStatistics Runtime::allocator_statistics() const noexcept {
    return allocator_.statistics();
}

Diagnostics& Runtime::diagnostics() noexcept {
    return diagnostics_;
}

strata_result Runtime::next_identity(std::uint64_t& identity) noexcept {
    const auto next = identities_.next();
    if (!next.has_value()) {
        return diagnostics_.emit(
            STRATA_STATUS_INVARIANT_FAILURE,
            STRATA_DIAGNOSTIC_FATAL,
            "STRATA.CORE.IDENTITY_EXHAUSTED",
            "The runtime-local stable identity space is exhausted."
        );
    }
    identity = *next;
    return result(STRATA_STATUS_OK);
}

strata_result Runtime::create_snapshot(SnapshotData& snapshot) noexcept {
    std::int64_t now = 0;
    try {
        now = clock_.now_nanoseconds(clock_.user_data);
    } catch (...) {
        return diagnostics_.emit(
            STRATA_STATUS_INTERNAL_ERROR,
            STRATA_DIAGNOSTIC_FATAL,
            "STRATA.ABI.CLOCK_CALLBACK_FAILED",
            "The caller-owned clock callback raised an exception."
        );
    }
    if (has_sampled_time_ && now < last_time_nanoseconds_) {
        return diagnostics_.emit(
            STRATA_STATUS_CLOCK_REGRESSION,
            STRATA_DIAGNOSTIC_ERROR,
            "STRATA.RUNTIME.CLOCK_REGRESSION",
            "The caller-owned monotonic clock moved backwards."
        );
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return diagnostics_.emit(
            STRATA_STATUS_INVARIANT_FAILURE,
            STRATA_DIAGNOSTIC_FATAL,
            "STRATA.CORE.GENERATION_EXHAUSTED",
            "The runtime snapshot generation is exhausted."
        );
    }
    last_time_nanoseconds_ = now;
    has_sampled_time_ = true;
    ++generation_;
    snapshot = SnapshotData{generation_, now, identities_.last()};
    return result(STRATA_STATUS_OK);
}

bool Runtime::publish_host_snapshot(
    std::string id,
    const std::uint64_t generation,
    const data::JsonValue& values
) {
    const std::shared_ptr<const runtime::HostSnapshot> snapshot =
        application_bundle_ != nullptr
            ? application_bundle_->host_snapshot(std::move(id), generation, values)
            : runtime::HostSnapshot::from_json(std::move(id), generation, values);
    const bool changed = host_store_.adopt(snapshot);
    if (application_ != nullptr) static_cast<void>(application_->host().adopt(snapshot));
    return changed;
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
    const data::JsonValue& registry,
    const data::JsonValue* const schemas,
    const std::span<const data::JsonValue> extension_declarations
) {
    if (application_ != nullptr) {
        throw std::logic_error("runtime application is already configured");
    }
    std::shared_ptr<const runtime::ApplicationBundle> bundle =
        runtime::ApplicationBundle::create(registry, schemas, {}, extension_declarations);
    auto application = std::make_unique<runtime::ApplicationContext>(
        std::move(id),
        bundle,
        [this](
            const runtime::RuntimeDiagnosticRecord& diagnostic,
            const std::uint64_t dropped_count
        ) {
            static_cast<void>(diagnostics_.publish(diagnostic, dropped_count));
        }
    );
    application_bundle_ = std::move(bundle);
    application_ = std::move(application);
}

bool Runtime::has_application() const noexcept { return application_ != nullptr; }

runtime::ApplicationContext& Runtime::application() {
    if (application_ == nullptr) throw std::logic_error("runtime application is not configured");
    return *application_;
}

const runtime::ApplicationContext& Runtime::application() const {
    if (application_ == nullptr) throw std::logic_error("runtime application is not configured");
    return *application_;
}

runtime::ActivationResult Runtime::compile_and_activate(
    const compiler::ModuleSource& entry,
    const compiler::ModuleLoader& loader,
    const std::uint64_t generation
) {
    auto profiler_frame = application().profiler().frame(generation);
    ArenaScope scratch_scope(scratch_arena_);
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
