#include <strata/strata.h>

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "core/allocator.hpp"
#include "core/runtime.hpp"
#include "core/utf8.hpp"
#include "data/json.hpp"
#include "runtime/value.hpp"

using namespace strata::abi_detail;

extern "C" {

uint32_t strata_abi_version(void) {
    return STRATA_ABI_VERSION_CURRENT;
}

strata_capabilities strata_capability_bits(void) {
    return capabilities;
}

strata_result strata_get_api_info(
    const uint32_t requested_abi,
    strata_api_info* const out_info
) {
    if (out_info == nullptr || out_info->struct_size < sizeof(strata_api_info)) {
        return invalid_argument();
    }
    if (requested_abi < STRATA_ABI_VERSION_MINIMUM ||
        requested_abi > STRATA_ABI_VERSION_CURRENT) {
        return strata::core::result(STRATA_STATUS_UNSUPPORTED_ABI);
    }
    *out_info = strata_api_info{
        sizeof(strata_api_info),
        STRATA_ABI_VERSION_CURRENT,
        STRATA_ABI_VERSION_MINIMUM,
        capabilities,
    };
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_create(
    const strata_runtime_config* const config,
    strata_runtime** const out_runtime
) {
    if (out_runtime != nullptr) {
        *out_runtime = nullptr;
    }
    if (config == nullptr || out_runtime == nullptr ||
        config->struct_size < sizeof(strata_runtime_config)) {
        return invalid_argument();
    }

    const strata_diagnostic_sink sink =
        config->diagnostics.struct_size == 0U ? empty_sink() : config->diagnostics;
    if (config->diagnostics.struct_size != 0U &&
        config->diagnostics.struct_size < sizeof(strata_diagnostic_sink)) {
        return strata::core::emit_without_runtime(
            empty_sink(),
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_DIAGNOSTIC_SINK",
            "A diagnostic sink with a non-zero size must provide the complete ABI v1 structure."
        );
    }
    if (config->abi_version < STRATA_ABI_VERSION_MINIMUM ||
        config->abi_version > STRATA_ABI_VERSION_CURRENT) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_UNSUPPORTED_ABI,
            "STRATA.ABI.UNSUPPORTED_VERSION",
            "The requested Strata ABI version is not supported by this library."
        );
    }
    if (config->reserved != 0U) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.RESERVED_FIELD",
            "Reserved ABI fields must be zero."
        );
    }
    if ((config->required_capabilities & ~capabilities) != 0U) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_UNSUPPORTED_CAPABILITY,
            "STRATA.ABI.UNSUPPORTED_CAPABILITY",
            "The runtime requires a capability this Strata library does not provide."
        );
    }
    if (config->clock.struct_size < sizeof(strata_clock) ||
        config->clock.now_nanoseconds == nullptr) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.CLOCK_REQUIRED",
            "Runtime creation requires a complete caller-owned monotonic clock."
        );
    }

    strata_allocator allocator_abi = config->allocator;
    if (allocator_abi.allocate == nullptr && allocator_abi.deallocate == nullptr &&
        (allocator_abi.struct_size == 0U ||
         allocator_abi.struct_size >= sizeof(strata_allocator))) {
        allocator_abi = strata::core::default_allocator();
    }
    if (!strata::core::valid_allocator(allocator_abi)) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_ALLOCATOR",
            "Allocator callbacks must be supplied as a complete allocate/deallocate pair."
        );
    }

    std::optional<strata::core::HostAllocator> allocator;
    try {
        allocator.emplace(allocator_abi);
    } catch (const std::bad_alloc&) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Runtime allocator telemetry could not be initialized."
        );
    } catch (...) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Runtime allocator initialization failed inside the C ABI exception boundary."
        );
    }
    void* const storage = allocator->allocate(sizeof(strata_runtime), alignof(strata_runtime));
    if (storage == nullptr) {
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "The runtime handle could not be allocated."
        );
    }
    try {
        *out_runtime = std::construct_at(
            static_cast<strata_runtime*>(storage),
            *allocator,
            config->clock,
            sink,
            config->stable_identity_seed
        );
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        allocator->deallocate(storage, sizeof(strata_runtime), alignof(strata_runtime));
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Runtime initialization exhausted the configured allocator."
        );
    } catch (...) {
        allocator->deallocate(storage, sizeof(strata_runtime), alignof(strata_runtime));
        return strata::core::emit_without_runtime(
            sink,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Runtime initialization failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_runtime_release(strata_runtime* const runtime) {
    if (runtime == nullptr) {
        return strata::core::result(STRATA_STATUS_OK);
    }
    if (!runtime->surfaces.empty()) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.RUNTIME.LIVE_SURFACES",
            "Runtime release requires every owned Surface to complete or explicitly abandon its release barrier first."
        );
    }
    const strata::core::HostAllocator allocator = runtime->core.allocator();
    std::destroy_at(runtime);
    allocator.deallocate(runtime, sizeof(strata_runtime), alignof(strata_runtime));
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_get_memory_info(
    const strata_runtime* const runtime,
    strata_runtime_memory_info* const out_info
) {
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(strata_runtime_memory_info)) {
        return invalid_argument();
    }
    const strata::core::AllocatorStatistics statistics = runtime->core.allocator_statistics();
    *out_info = strata_runtime_memory_info{
        sizeof(strata_runtime_memory_info),
        statistics.routed.current_bytes,
        statistics.routed.peak_bytes,
        statistics.routed.total_bytes,
        statistics.routed.live_allocations,
        statistics.routed.peak_live_allocations,
        statistics.routed.allocation_count,
        statistics.routed.deallocation_count,
        statistics.arena.current_bytes,
        statistics.arena.peak_bytes,
        statistics.arena.total_bytes,
        statistics.arena.live_allocations,
        statistics.arena.peak_live_allocations,
        statistics.arena.allocation_count,
        statistics.arena.deallocation_count,
    };
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_next_identity(
    strata_runtime* const runtime,
    uint64_t* const out_identity
) {
    if (runtime == nullptr) {
        return invalid_argument();
    }
    if (out_identity == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.NULL_OUTPUT",
            "The stable identity output pointer must not be null."
        );
    }
    *out_identity = 0U;
    try {
        return runtime->core.next_identity(*out_identity);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Stable identity allocation exhausted the configured allocator."
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Stable identity allocation failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_runtime_create_snapshot(
    strata_runtime* const runtime,
    strata_snapshot** const out_snapshot
) {
    if (out_snapshot != nullptr) {
        *out_snapshot = nullptr;
    }
    if (runtime == nullptr) {
        return invalid_argument();
    }
    if (out_snapshot == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.NULL_OUTPUT",
            "The snapshot output pointer must not be null."
        );
    }

    const strata::core::HostAllocator allocator = runtime->core.allocator();
    void* const storage = allocator.allocate(sizeof(strata_snapshot), alignof(strata_snapshot));
    if (storage == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "The immutable snapshot handle could not be allocated."
        );
    }
    strata::core::SnapshotData data{};
    try {
        const strata_result snapshot_result = runtime->core.create_snapshot(data);
        if (snapshot_result.status != STRATA_STATUS_OK) {
            allocator.deallocate(storage, sizeof(strata_snapshot), alignof(strata_snapshot));
            return snapshot_result;
        }
        *out_snapshot = std::construct_at(
            static_cast<strata_snapshot*>(storage),
            allocator,
            data
        );
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        allocator.deallocate(storage, sizeof(strata_snapshot), alignof(strata_snapshot));
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Snapshot creation exhausted the configured allocator."
        );
    } catch (...) {
        allocator.deallocate(storage, sizeof(strata_snapshot), alignof(strata_snapshot));
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Snapshot creation failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_snapshot_get_info(
    const strata_snapshot* const snapshot,
    strata_snapshot_info* const out_info
) {
    if (snapshot == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(strata_snapshot_info)) {
        return invalid_argument();
    }
    *out_info = strata_snapshot_info{
        sizeof(strata_snapshot_info),
        snapshot->data.generation,
        snapshot->data.time_nanoseconds,
        snapshot->data.last_stable_identity,
    };
    return strata::core::result(STRATA_STATUS_OK);
}

void strata_snapshot_release(strata_snapshot* const snapshot) {
    if (snapshot == nullptr) {
        return;
    }
    const strata::core::HostAllocator allocator = snapshot->allocator;
    std::destroy_at(snapshot);
    allocator.deallocate(snapshot, sizeof(strata_snapshot), alignof(strata_snapshot));
}

} // extern "C"
