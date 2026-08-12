#include <strata/strata.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "core/utf8.hpp"
#include "data/json.hpp"

using namespace strata::abi_detail;

strata_result strata_runtime_set_resource_adapter(strata_runtime* const runtime,
                                                  const strata_resource_adapter* const adapter) {
    if (runtime == nullptr)
        return invalid_argument();
    if (adapter == nullptr) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.RESOURCE_ADAPTER_REQUIRED",
                               "A resource adapter cannot be null.");
    }
    if (adapter->struct_size < sizeof(strata_resource_adapter) || adapter->load == nullptr) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_RESOURCE_ADAPTER",
            "A resource adapter requires the complete ABI structure and a load callback.");
    }
    if (runtime->resources.has_value()) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.RESOURCE.IMMUTABLE_ADAPTER",
                               "The resource adapter is immutable after installation; create a new "
                               "Runtime to change it.");
    }
    runtime->resources = *adapter;
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_read_resource(strata_runtime* const runtime,
                                           const strata_string_view resource_id,
                                           const strata_bytes_sink* const sink) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!valid_view(resource_id, false) || sink == nullptr ||
        sink->struct_size < sizeof(strata_bytes_sink) || sink->emit == nullptr) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_RESOURCE_READ",
            "Resource reads require a non-empty UTF-8 id and complete byte sink.");
    }
    if (!runtime->resources.has_value()) {
        return runtime_failure(*runtime, STRATA_STATUS_SERVICE_UNAVAILABLE,
                               "STRATA.RESOURCE.ADAPTER_UNAVAILABLE",
                               "The runtime has no configured resource adapter.");
    }
    try {
        const std::string id = copied_string(resource_id);
        if (!strata::core::valid_utf8(id))
            throw std::invalid_argument("resource id must be valid UTF-8");
        strata_bytes_view borrowed{nullptr, 0U};
        const strata_status status = runtime->resources->load(
            runtime->resources->user_data, strata_string_view{id.data(), id.size()}, &borrowed);
        if (status != STRATA_STATUS_OK) {
            return runtime_failure(*runtime, status, "STRATA.RESOURCE.LOAD_FAILED",
                                   "The host resource adapter rejected the requested resource.");
        }
        if (borrowed.data == nullptr && borrowed.size != 0U) {
            throw std::invalid_argument("resource adapter returned a null non-empty byte view");
        }
        std::vector<std::uint8_t> copied;
        if (borrowed.size != 0U)
            copied.assign(borrowed.data, borrowed.data + borrowed.size);
        sink->emit(sink->user_data, strata_bytes_view{copied.data(), copied.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(*runtime, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY",
                               "Resource adapter transfer exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.RESOURCE.INVALID_RESULT", error.what());
    } catch (...) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.CALLBACK_FAILED",
            "Resource loading or its byte sink failed inside the C ABI boundary.");
    }
}

strata_result strata_runtime_set_clipboard_adapter(strata_runtime* const runtime,
                                                   const strata_clipboard_adapter* const adapter) {
    if (runtime == nullptr)
        return invalid_argument();
    if (adapter == nullptr) {
        runtime->clipboard.reset();
        return strata::core::result(STRATA_STATUS_OK);
    }
    if (adapter->struct_size < sizeof(strata_clipboard_adapter) || adapter->read == nullptr ||
        adapter->write == nullptr) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_CLIPBOARD_ADAPTER",
                               "A clipboard adapter requires complete read and write callbacks.");
    }
    runtime->clipboard = *adapter;
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_clipboard_read(strata_runtime* const runtime,
                                            const strata_string_sink* const text_sink) {
    if (runtime == nullptr)
        return invalid_argument();
    if (text_sink == nullptr || text_sink->struct_size < sizeof(strata_string_sink) ||
        text_sink->emit == nullptr) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_STRING_SINK",
                               "Clipboard reads require a complete UTF-8 string sink.");
    }
    if (!runtime->clipboard.has_value()) {
        return runtime_failure(*runtime, STRATA_STATUS_SERVICE_UNAVAILABLE,
                               "STRATA.CLIPBOARD.ADAPTER_UNAVAILABLE",
                               "The runtime has no configured clipboard adapter.");
    }
    try {
        strata_string_view borrowed{nullptr, 0U};
        const strata_status status =
            runtime->clipboard->read(runtime->clipboard->user_data, &borrowed);
        if (status != STRATA_STATUS_OK) {
            return runtime_failure(*runtime, status, "STRATA.CLIPBOARD.READ_FAILED",
                                   "The host clipboard adapter rejected a read.");
        }
        if (!valid_view(borrowed, true))
            throw std::invalid_argument("clipboard adapter returned an invalid string view");
        const std::string copied = copied_string(borrowed);
        if (!strata::core::valid_utf8(copied))
            throw std::invalid_argument("clipboard adapter returned invalid UTF-8");
        text_sink->emit(text_sink->user_data, strata_string_view{copied.data(), copied.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(*runtime, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY",
                               "Clipboard read exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_UTF8,
                               "STRATA.CLIPBOARD.INVALID_TEXT", error.what());
    } catch (...) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.CALLBACK_FAILED",
            "Clipboard read or its string sink failed inside the C ABI boundary.");
    }
}

strata_result strata_runtime_clipboard_write(strata_runtime* const runtime,
                                             const strata_string_view text) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!valid_view(text, true)) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_CLIPBOARD_TEXT",
                               "Clipboard writes require a valid borrowed string view.");
    }
    if (!runtime->clipboard.has_value()) {
        return runtime_failure(*runtime, STRATA_STATUS_SERVICE_UNAVAILABLE,
                               "STRATA.CLIPBOARD.ADAPTER_UNAVAILABLE",
                               "The runtime has no configured clipboard adapter.");
    }
    try {
        const std::string copied = copied_string(text);
        if (!strata::core::valid_utf8(copied)) {
            return runtime_failure(*runtime, STRATA_STATUS_INVALID_UTF8,
                                   "STRATA.CLIPBOARD.INVALID_TEXT",
                                   "Clipboard text must be valid UTF-8.");
        }
        const strata_status status = runtime->clipboard->write(
            runtime->clipboard->user_data, strata_string_view{copied.data(), copied.size()});
        return status == STRATA_STATUS_OK
                   ? strata::core::result(STRATA_STATUS_OK)
                   : runtime_failure(*runtime, status, "STRATA.CLIPBOARD.WRITE_FAILED",
                                     "The host clipboard adapter rejected a write.");
    } catch (...) {
        return runtime_failure(*runtime, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.CALLBACK_FAILED",
                               "Clipboard write failed inside the C ABI boundary.");
    }
}

strata_result strata_runtime_set_ime_adapter(strata_runtime* const runtime,
                                             const strata_ime_adapter* const adapter) {
    if (runtime == nullptr)
        return invalid_argument();
    if (adapter != nullptr &&
        (adapter->struct_size < sizeof(strata_ime_adapter) || adapter->set_active == nullptr ||
         adapter->set_cursor_rect == nullptr)) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_IME_ADAPTER",
            "An IME adapter requires active-state and cursor-rectangle callbacks.");
    }
    runtime->host_services.adapters_changed();
    if (adapter == nullptr) {
        runtime->ime.reset();
        for (strata_surface* const surface : runtime->surfaces) {
            if (surface != nullptr && !surface->release_packet_prepared) {
                surface->core.invalidate();
            }
        }
        return strata::core::result(STRATA_STATUS_OK);
    }
    runtime->ime = *adapter;
    for (strata_surface* const surface : runtime->surfaces) {
        if (surface != nullptr && !surface->release_packet_prepared) {
            surface->core.invalidate();
        }
    }
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_ime_set_active(strata_runtime* const runtime, const uint32_t active) {
    if (runtime == nullptr)
        return invalid_argument();
    if (active > 1U) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_BOOLEAN",
                               "IME active state must be zero or one.");
    }
    if (!runtime->ime.has_value()) {
        return runtime_failure(*runtime, STRATA_STATUS_SERVICE_UNAVAILABLE,
                               "STRATA.IME.ADAPTER_UNAVAILABLE",
                               "The runtime has no configured IME adapter.");
    }
    try {
        const strata_status status = runtime->ime->set_active(runtime->ime->user_data, active);
        return status == STRATA_STATUS_OK
                   ? strata::core::result(STRATA_STATUS_OK)
                   : runtime_failure(*runtime, status, "STRATA.IME.UPDATE_FAILED",
                                     "The host IME adapter rejected active state.");
    } catch (...) {
        return runtime_failure(*runtime, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.CALLBACK_FAILED",
                               "IME active-state update failed inside the C ABI boundary.");
    }
}

strata_result strata_runtime_ime_set_cursor_rect(strata_runtime* const runtime,
                                                 const strata_rect logical_rect) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!std::isfinite(logical_rect.x) || !std::isfinite(logical_rect.y) ||
        !std::isfinite(logical_rect.width) || !std::isfinite(logical_rect.height) ||
        logical_rect.width < 0.0 || logical_rect.height < 0.0) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_GEOMETRY",
            "IME cursor geometry must contain finite coordinates and non-negative extents.");
    }
    if (!runtime->ime.has_value()) {
        return runtime_failure(*runtime, STRATA_STATUS_SERVICE_UNAVAILABLE,
                               "STRATA.IME.ADAPTER_UNAVAILABLE",
                               "The runtime has no configured IME adapter.");
    }
    try {
        const strata_status status =
            runtime->ime->set_cursor_rect(runtime->ime->user_data, logical_rect);
        return status == STRATA_STATUS_OK
                   ? strata::core::result(STRATA_STATUS_OK)
                   : runtime_failure(*runtime, status, "STRATA.IME.UPDATE_FAILED",
                                     "The host IME adapter rejected cursor geometry.");
    } catch (...) {
        return runtime_failure(*runtime, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.CALLBACK_FAILED",
                               "IME cursor update failed inside the C ABI boundary.");
    }
}

strata_result strata_runtime_set_effect_adapter(strata_runtime* const runtime,
                                                const strata_effect_adapter* const adapter) {
    if (runtime == nullptr)
        return invalid_argument();
    if (adapter == nullptr) {
        runtime->effects.reset();
        return strata::core::result(STRATA_STATUS_OK);
    }
    if (adapter->struct_size < sizeof(strata_effect_adapter) || adapter->emit == nullptr) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_EFFECT_ADAPTER",
            "An effect adapter requires the complete ABI structure and emit callback.");
    }
    runtime->effects = *adapter;
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_emit_effect_json(strata_runtime* const runtime,
                                              const strata_string_view effect_id,
                                              const strata_string_view payload_json) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!valid_view(effect_id, false) || !valid_view(payload_json, true)) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_EFFECT",
            "Effect emission requires a non-empty id and optional JSON payload.");
    }
    if (!runtime->effects.has_value()) {
        return runtime_failure(*runtime, STRATA_STATUS_SERVICE_UNAVAILABLE,
                               "STRATA.EFFECT.ADAPTER_UNAVAILABLE",
                               "The runtime has no configured effect adapter.");
    }
    try {
        const std::string id = copied_string(effect_id);
        if (!strata::core::valid_utf8(id))
            throw std::invalid_argument("effect id must be valid UTF-8");
        const strata::data::JsonValue payload =
            payload_json.size == 0U ? strata::data::JsonValue{}
                                    : strata::data::parse_json(copied_string(payload_json));
        const std::string canonical = strata::data::encode_canonical_json(payload);
        const strata_status status =
            static_cast<strata_status>(runtime->host_services.emit_effect(id, canonical));
        return status == STRATA_STATUS_OK
                   ? strata::core::result(STRATA_STATUS_OK)
                   : runtime_failure(*runtime, status, "STRATA.EFFECT.EMIT_FAILED",
                                     "The host effect adapter rejected an effect.");
    } catch (const strata::data::JsonError&) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.EFFECT.INVALID_JSON",
                               "Effect payload is not valid strict JSON.");
    } catch (const std::invalid_argument& error) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_UTF8, "STRATA.EFFECT.INVALID_ID",
                               error.what());
    } catch (const std::bad_alloc&) {
        return runtime_failure(*runtime, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY",
                               "Effect emission exhausted memory.");
    } catch (...) {
        return runtime_failure(*runtime, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.CALLBACK_FAILED",
                               "Effect emission failed inside the C ABI boundary.");
    }
}

strata_result
strata_runtime_set_durable_store_adapter(strata_runtime* const runtime,
                                         const strata_durable_store_adapter* const adapter) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application() || runtime->durable_store.has_value() ||
        !runtime->surfaces.empty() || adapter == nullptr ||
        adapter->struct_size < sizeof(strata_durable_store_adapter) || adapter->load == nullptr ||
        adapter->write == nullptr) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_DURABLE_STORE_ADAPTER",
            "Durable store installation is one-time and requires a configured application, no "
            "created Surface, and complete load/write callbacks.");
    }
    runtime->durable_store = *adapter;
    runtime->core.application().configure_durable_store(strata::runtime::DurableStoreAdapter{
        [runtime](const std::string_view application_id) -> std::optional<std::string> {
            strata_bytes_view bytes{nullptr, 0U};
            const strata_status status = runtime->durable_store->load(
                runtime->durable_store->user_data,
                strata_string_view{application_id.data(), application_id.size()}, &bytes);
            if (status == STRATA_STATUS_NOT_FOUND)
                return std::nullopt;
            if (status != STRATA_STATUS_OK) {
                throw std::runtime_error("host durable load returned status " +
                                         std::to_string(status));
            }
            if (bytes.data == nullptr && bytes.size != 0U) {
                throw std::invalid_argument("durable load returned null non-empty bytes");
            }
            std::string copied =
                bytes.size == 0U
                    ? std::string{}
                    : std::string(reinterpret_cast<const char*>(bytes.data),
                                  reinterpret_cast<const char*>(bytes.data) + bytes.size);
            if (!strata::core::valid_utf8(copied)) {
                throw std::invalid_argument("durable payload must be UTF-8");
            }
            return copied;
        },
        [runtime](const std::string_view application_id, const std::string_view payload) {
            const strata_status status = runtime->durable_store->write(
                runtime->durable_store->user_data,
                strata_string_view{application_id.data(), application_id.size()},
                strata_bytes_view{
                    reinterpret_cast<const std::uint8_t*>(payload.data()),
                    payload.size(),
                });
            if (status != STRATA_STATUS_OK) {
                throw std::runtime_error("host durable write returned status " +
                                         std::to_string(status));
            }
        },
    });
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result
strata_runtime_read_durable_shell_value_json(strata_runtime* const runtime,
                                             const strata_string_view key,
                                             const strata_value_json_sink* const sink) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application() || !valid_view(key, false) || sink == nullptr ||
        sink->struct_size < sizeof(strata_value_json_sink) || sink->emit == nullptr) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_DURABLE_READ",
            "Durable shell reads require a configured application, key, and complete JSON sink.");
    }
    const std::string copied_key = copied_string(key);
    const strata::runtime::Value* value =
        runtime->core.application().durability().shell_value(copied_key);
    if (value == nullptr)
        return strata::core::result(STRATA_STATUS_NOT_FOUND);
    try {
        const std::string json =
            strata::data::encode_canonical_json(strata::runtime::value_to_json(*value));
        sink->emit(sink->user_data, strata_string_view{json.data(), json.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return runtime_failure(*runtime, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.CALLBACK_FAILED",
                               "Durable shell JSON emission failed inside the C ABI boundary.");
    }
}

strata_result strata_runtime_write_durable_shell_value_json(strata_runtime* const runtime,
                                                            const strata_string_view key,
                                                            const strata_string_view value_json) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application() || !valid_view(key, false) ||
        !valid_view(value_json, false)) {
        return runtime_failure(
            *runtime, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_DURABLE_WRITE",
            "Durable shell writes require a configured application, key, and value JSON.");
    }
    try {
        runtime->core.application().durability().set_shell_value(
            copied_string(key),
            strata::runtime::value_from_json(strata::data::parse_json(copied_string(value_json))));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::exception& error) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.DURABILITY.INVALID_SHELL_VALUE", error.what());
    }
}

strata_result strata_runtime_flush_durable_state(strata_runtime* const runtime) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application()) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.APPLICATION_REQUIRED",
                               "Durable state flush requires a configured application.");
    }
    const std::optional<strata::runtime::DurableLoadIssue> issue =
        runtime->core.application().durability().flush();
    return issue.has_value() ? runtime_failure(*runtime, STRATA_STATUS_INTERNAL_ERROR,
                                               issue->code.c_str(), issue->message.c_str())
                             : strata::core::result(STRATA_STATUS_OK);
}

strata_result
strata_runtime_set_async_host_adapter(strata_runtime* const runtime,
                                      const strata_async_host_adapter* const adapter) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application() || runtime->async_host.has_value() || adapter == nullptr ||
        adapter->struct_size < sizeof(strata_async_host_adapter) || adapter->begin == nullptr ||
        adapter->cancel == nullptr) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_ASYNC_HOST_ADAPTER",
                               "Async host installation is one-time and requires a configured "
                               "application and complete begin/cancel callbacks.");
    }
    runtime->async_host = *adapter;
    runtime->core.application().configure_async_host(strata::runtime::AsyncHostAdapter{
        [runtime](const strata::runtime::AsyncRequest& request) {
            const std::string payload = strata::data::encode_canonical_json(
                strata::runtime::value_to_json(request.payload));
            const strata_status status = runtime->async_host->begin(
                runtime->async_host->user_data, request.id,
                strata_string_view{request.binding.data(), request.binding.size()},
                strata_string_view{request.owner.data(), request.owner.size()},
                strata_string_view{payload.data(), payload.size()});
            if (status != STRATA_STATUS_OK) {
                static_cast<void>(runtime->core.application().async().fail(
                    request.id, strata::runtime::AsyncFailure{
                                    "The host rejected the asynchronous query.",
                                    "HOST_STATUS_" + std::to_string(status),
                                }));
            }
        },
        [runtime](const std::uint64_t request_id) {
            runtime->async_host->cancel(runtime->async_host->user_data, request_id);
        },
    });
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_async_progress(strata_runtime* const runtime,
                                            const std::uint64_t request_id,
                                            const strata_async_progress* const progress) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application() || progress == nullptr ||
        progress->struct_size < sizeof(strata_async_progress) || progress->has_total > 1U ||
        progress->reserved != 0U || !valid_view(progress->message, true)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const bool accepted = runtime->core.application().async().post_progress(
            request_id,
            strata::runtime::AsyncProgress{
                progress->completed,
                progress->has_total != 0U ? std::optional<double>(progress->total) : std::nullopt,
                copied_string(progress->message),
            });
        return strata::core::result(accepted ? STRATA_STATUS_OK : STRATA_STATUS_INVALID_ARGUMENT);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}

strata_result strata_runtime_async_succeed_json(strata_runtime* const runtime,
                                                const std::uint64_t request_id,
                                                const strata_string_view value_json) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application() || !valid_view(value_json, false)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const bool accepted = runtime->core.application().async().post_succeed(
            request_id,
            strata::runtime::value_from_json(strata::data::parse_json(copied_string(value_json))));
        return strata::core::result(accepted ? STRATA_STATUS_OK : STRATA_STATUS_INVALID_ARGUMENT);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_runtime_async_fail(strata_runtime* const runtime,
                                        const std::uint64_t request_id,
                                        const strata_string_view message,
                                        const strata_string_view code) {
    if (runtime == nullptr)
        return invalid_argument();
    if (!runtime->core.has_application() || !valid_view(message, false) ||
        !valid_view(code, true)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const bool accepted = runtime->core.application().async().post_fail(
            request_id, strata::runtime::AsyncFailure{copied_string(message), copied_string(code)});
        return strata::core::result(accepted ? STRATA_STATUS_OK : STRATA_STATUS_INVALID_ARGUMENT);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}
