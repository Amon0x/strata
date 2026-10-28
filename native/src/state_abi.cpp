#include <strata/strata.h>

#include <memory>
#include <new>

#include "abi_internal.hpp"
#include "abi_support.hpp"

using strata::abi_detail::EntryGuard;
using strata::abi_detail::Reentry;

extern "C" {

strata_result strata_runtime_create_application_state_snapshot(
    strata_runtime* const runtime,
    strata_application_state_snapshot** const out_snapshot
) noexcept {
    if (out_snapshot != nullptr) *out_snapshot = nullptr;
    const EntryGuard entry(runtime, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (runtime == nullptr || out_snapshot == nullptr || !runtime->core.has_application()) {
        return strata::abi_detail::invalid_argument();
    }
    const strata::base::HostAllocator allocator = runtime->core.allocator();
    void* const storage = allocator.allocate(
        sizeof(strata_application_state_snapshot),
        alignof(strata_application_state_snapshot)
    );
    if (storage == nullptr) {
        return strata::abi_detail::runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "The application state snapshot handle could not be allocated."
        );
    }
    constexpr strata::abi_detail::AbiCall call{"Application state snapshot creation"};
    return strata::abi_detail::runtime_call(runtime, call, [&] {
        try {
            *out_snapshot = std::construct_at(
                static_cast<strata_application_state_snapshot*>(storage),
                allocator,
                runtime->core.application().state().snapshot()
            );
            return strata::abi_detail::result(STRATA_STATUS_OK);
        } catch (...) {
            allocator.deallocate(
                storage,
                sizeof(strata_application_state_snapshot),
                alignof(strata_application_state_snapshot)
            );
            throw;
        }
    });
}

strata_result strata_runtime_restore_application_state(
    strata_runtime* const runtime,
    const strata_application_state_snapshot* const snapshot,
    uint32_t* const out_changed
) noexcept {
    if (out_changed != nullptr) *out_changed = 0U;
    const EntryGuard entry(runtime, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (runtime == nullptr || snapshot == nullptr || out_changed == nullptr ||
        !runtime->core.has_application()) {
        return strata::abi_detail::invalid_argument();
    }
    constexpr strata::abi_detail::AbiCall call{"Application state restore"};
    return strata::abi_detail::runtime_call(runtime, call, [&] {
        *out_changed = runtime->core.application().state().restore(snapshot->data) ? 1U : 0U;
        if (*out_changed != 0U) {
            runtime->core.application().undo().clear_all();
            runtime->core.application().synchronize_durable_state();
        }
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

void strata_application_state_snapshot_release(
    strata_application_state_snapshot* const snapshot
) noexcept {
    if (snapshot == nullptr) return;
    const strata::base::HostAllocator allocator = snapshot->allocator;
    std::destroy_at(snapshot);
    allocator.deallocate(
        snapshot,
        sizeof(strata_application_state_snapshot),
        alignof(strata_application_state_snapshot)
    );
}

} // extern "C"
