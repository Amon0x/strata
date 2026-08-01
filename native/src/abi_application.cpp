#include <strata/strata.h>

#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "compiler/artifact.hpp"
#include "data/json.hpp"
#include "diagnostic_abi.hpp"
#include "runtime/application.hpp"
#include "ui/render/material_registry.hpp"
#include "runtime/value.hpp"

using namespace strata::abi_detail;

namespace {

[[nodiscard]] bool valid_json_sink(const strata_value_json_sink* const sink) noexcept {
    return sink != nullptr && sink->struct_size >= sizeof(strata_value_json_sink) &&
           sink->emit != nullptr;
}

[[nodiscard]] strata_result finish_activation(
    strata_runtime& runtime,
    const strata::runtime::ActivationResult& activation,
    strata_activation_info& out_info
) {
    out_info = strata_activation_info{
        sizeof(strata_activation_info),
        activation_status(activation.status),
        activation.state_migrated ? 1U : 0U,
        activation.active_generation.has_value() ? 1U : 0U,
        0U,
        activation.attempted_generation,
        activation.active_generation.value_or(0U),
        static_cast<std::uint64_t>(activation.diagnostics.size()),
    };
    if (activation.activated()) return strata::core::result(STRATA_STATUS_OK);

    const strata_status status =
        activation.status == strata::runtime::ActivationStatus::rejected_compile
        ? STRATA_STATUS_COMPILE_FAILED
        : STRATA_STATUS_ACTIVATION_REJECTED;
    strata_result result = strata::core::result(status);
    for (const strata::runtime::ActivationDiagnostic& diagnostic : activation.diagnostics) {
        const std::optional<strata_source_range> range = diagnostic.range.has_value()
            ? std::optional(strata_source_range{
                  diagnostic.range->start.offset.value_or(0U),
                  diagnostic.range->end.offset.value_or(0U),
                  diagnostic.range->start.line,
                  diagnostic.range->start.column,
                  diagnostic.range->end.line,
                  diagnostic.range->end.column,
              })
            : std::nullopt;
        const std::string_view source_id = diagnostic.range.has_value()
            ? std::string_view(diagnostic.range->source_id)
            : diagnostic.source_id.has_value()
                ? std::string_view(*diagnostic.source_id)
                : std::string_view{};
        const strata_diagnostic_severity severity = [&diagnostic] {
            switch (diagnostic.severity) {
            case strata::runtime::DiagnosticSeverity::info: return STRATA_DIAGNOSTIC_INFO;
            case strata::runtime::DiagnosticSeverity::warning: return STRATA_DIAGNOSTIC_WARNING;
            case strata::runtime::DiagnosticSeverity::error: return STRATA_DIAGNOSTIC_ERROR;
            }
            return STRATA_DIAGNOSTIC_ERROR;
        }();
        result = runtime.core.diagnostics().emit(
            status,
            severity,
            diagnostic.code,
            diagnostic.message,
            source_id,
            range.has_value() ? &*range : nullptr,
            diagnostic.component_path.has_value()
                ? std::string_view(*diagnostic.component_path)
                : std::string_view{},
            diagnostic.expected.has_value()
                ? std::string_view(*diagnostic.expected)
                : std::string_view{}
        );
    }
    if (activation.diagnostics.empty()) {
        result = runtime.core.diagnostics().emit(
            status,
            STRATA_DIAGNOSTIC_ERROR,
            "STRATA.RUNTIME.ACTIVATION_REJECTED",
            "The candidate unit was rejected; the last-good unit remains active."
        );
    }
    return result;
}

} // namespace

strata_result strata_runtime_publish_host_snapshot(
    strata_runtime* const runtime,
    const strata_host_snapshot_config* const snapshot
) {
    if (runtime == nullptr) return invalid_argument();
    if (snapshot == nullptr || snapshot->struct_size < sizeof(strata_host_snapshot_config) ||
        !valid_view(snapshot->id, false) || !valid_view(snapshot->values_json, false)) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_HOST_SNAPSHOT",
            "A host snapshot requires a complete structure, non-empty id, and JSON object."
        );
    }
    try {
        const std::string json_text = copied_string(snapshot->values_json);
        const strata::data::JsonValue values = strata::data::parse_json(json_text);
        if (values.object() == nullptr) {
            return runtime_failure(
                *runtime,
                STRATA_STATUS_INVALID_ARGUMENT,
                "STRATA.HOST.INVALID_ROOTS",
                "Host snapshot values must be one JSON object."
            );
        }
        static_cast<void>(runtime->core.publish_host_snapshot(
            copied_string(snapshot->id),
            snapshot->generation,
            values
        ));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Host snapshot publication exhausted memory."
        );
    } catch (const strata::data::JsonError&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.HOST.INVALID_JSON",
            "Host snapshot values are not valid strict JSON."
        );
    } catch (const std::invalid_argument& error) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.HOST.INVALID_SNAPSHOT",
            error.what()
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Host snapshot publication failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_runtime_get_host_snapshot_info(
    const strata_runtime* const runtime,
    strata_host_snapshot_info* const out_info
) {
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(strata_host_snapshot_info)) {
        return invalid_argument();
    }
    const auto& snapshot = runtime->core.host_snapshot();
    *out_info = strata_host_snapshot_info{
        sizeof(strata_host_snapshot_info),
        snapshot != nullptr ? snapshot->generation() : 0U,
        snapshot != nullptr ? snapshot->evaluated_scalar_count() : 0U,
        snapshot != nullptr ? 1U : 0U,
        0U,
    };
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_get_host_snapshot_generation(
    const strata_runtime* const runtime,
    const strata_string_view id,
    std::uint64_t* const out_generation
) {
    if (runtime == nullptr || out_generation == nullptr || !valid_view(id, false)) {
        return invalid_argument();
    }
    const std::optional<std::uint64_t> generation =
        runtime->core.host_snapshot_generation(copied_string(id));
    *out_generation = generation.value_or(0U);
    return strata::core::result(
        generation.has_value() ? STRATA_STATUS_OK : STRATA_STATUS_NOT_FOUND
    );
}

strata_result strata_runtime_read_diagnostics(
    const strata_runtime* const runtime,
    const strata_diagnostics_snapshot_sink* const sink
) {
    if (runtime == nullptr || sink == nullptr ||
        sink->struct_size < sizeof(strata_diagnostics_snapshot_sink) || sink->emit == nullptr) {
        return invalid_argument();
    }
    try {
        const strata::runtime::RuntimeDiagnosticsSnapshot snapshot = runtime->core.has_application()
            ? runtime->core.application().services().diagnostics_snapshot()
            : strata::runtime::RuntimeDiagnosticsSnapshot{};
        emit_diagnostics_snapshot(snapshot, *sink);
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}

strata_result strata_runtime_clear_diagnostics(strata_runtime* const runtime) {
    if (runtime == nullptr) return invalid_argument();
    runtime->core.diagnostics().clear();
    if (runtime->core.has_application()) {
        runtime->core.application().services().clear_diagnostics();
    }
    for (strata_surface* const surface : runtime->surfaces) {
        if (surface == nullptr) continue;
        surface->core.clear_diagnostics();
        surface->frame_json.clear();
    }
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_runtime_read_host_value_json(
    strata_runtime* const runtime,
    const strata_string_view path,
    const strata_value_json_sink* const sink
) {
    if (runtime == nullptr) return invalid_argument();
    if (!valid_view(path, false) || sink == nullptr ||
        sink->struct_size < sizeof(strata_value_json_sink) || sink->emit == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_VALUE_SINK",
            "Host value reads require a non-empty path and complete JSON sink."
        );
    }
    try {
        const std::optional<strata::runtime::Value> value =
            runtime->core.read_host_value(std::string_view(path.data, path.size));
        if (!value.has_value()) {
            return runtime_failure(
                *runtime,
                STRATA_STATUS_NOT_FOUND,
                "STRATA.HOST.PATH_NOT_FOUND",
                "The requested host value path is not present in the active snapshot."
            );
        }
        const std::string encoded = strata::data::encode_canonical_json(
            strata::runtime::value_to_json(*value)
        );
        sink->emit(sink->user_data, strata_string_view{encoded.data(), encoded.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Host value evaluation exhausted memory."
        );
    } catch (const std::invalid_argument& error) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.HOST.INVALID_PATH",
            error.what()
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Host value evaluation or its JSON sink failed inside the C ABI boundary."
        );
    }
}

strata_result strata_runtime_configure_application(
    strata_runtime* const runtime,
    const strata_application_config* const config
) {
    if (runtime == nullptr) return invalid_argument();
    if (config == nullptr || config->struct_size < sizeof(strata_application_config) ||
        !valid_view(config->id, false) || !valid_view(config->registry_json, false) ||
        !valid_view(config->schemas_json, true) ||
        (config->extension_schemas_json == nullptr && config->extension_schema_count != 0U)) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_APPLICATION_CONFIG",
            "Application configuration requires an id, neutral registry JSON, and an optional schemas JSON object."
        );
    }
    try {
        const strata::data::JsonValue registry =
            strata::data::parse_json(copied_string(config->registry_json));
        std::optional<strata::data::JsonValue> schemas;
        if (config->schemas_json.size != 0U) {
            schemas = strata::data::parse_json(copied_string(config->schemas_json));
            if (schemas->object() == nullptr) {
                throw std::invalid_argument("application schemas must be a JSON object");
            }
        }
        std::vector<strata::data::JsonValue> extension_declarations;
        extension_declarations.reserve(config->extension_schema_count);
        for (std::size_t index = 0U; index < config->extension_schema_count; ++index) {
            const strata_string_view document = config->extension_schemas_json[index];
            if (!valid_view(document, false)) {
                throw std::invalid_argument("extension schema documents must not be empty");
            }
            strata::data::JsonValue parsed = strata::data::parse_json(copied_string(document));
            if (parsed.object() == nullptr) {
                throw std::invalid_argument("extension schema documents must be JSON objects");
            }
            extension_declarations.push_back(std::move(parsed));
        }
        runtime->core.configure_application(
            copied_string(config->id),
            registry,
            schemas.has_value() ? &*schemas : nullptr,
            extension_declarations
        );
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Application configuration exhausted memory."
        );
    } catch (const strata::data::JsonError&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.APPLICATION.INVALID_JSON",
            "Application registry or schemas are not valid strict JSON."
        );
    } catch (const std::exception& error) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.APPLICATION.INVALID_CONFIGURATION",
            error.what()
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Application configuration failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_runtime_compile_and_activate(
    strata_runtime* const runtime,
    const strata_activation_config* const config,
    strata_activation_info* const out_info
) {
    if (runtime == nullptr) return invalid_argument();
    if (out_info == nullptr || out_info->struct_size < sizeof(strata_activation_info) ||
        config == nullptr || config->struct_size < sizeof(strata_activation_config) ||
        !valid_view(config->entry_source_id, false) || !valid_view(config->entry_text, true)) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_ACTIVATION_CONFIG",
            "Activation requires a complete configuration, entry source id/text, and output structure."
        );
    }
    *out_info = strata_activation_info{sizeof(strata_activation_info), 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    if (!runtime->core.has_application()) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.APPLICATION.NOT_CONFIGURED",
            "Configure an application before compiling and activating a unit."
        );
    }
    try {
        const strata::compiler::ModuleLoader loader = [config](
                                                          const std::string_view importer,
                                                          const std::string_view path
                                                      ) {
            if (config->load_module == nullptr) {
                throw strata::compiler::ModuleLoadError(
                    "Imported module '" + std::string(path) + "' requires a host module loader.",
                    std::string(path)
                );
            }
            strata_module_source source{
                sizeof(strata_module_source),
                strata_string_view{nullptr, 0U},
                strata_string_view{nullptr, 0U},
            };
            const strata_status status = config->load_module(
                config->loader_user_data,
                strata_string_view{importer.data(), importer.size()},
                strata_string_view{path.data(), path.size()},
                &source
            );
            if (status != STRATA_STATUS_OK || source.struct_size < sizeof(strata_module_source) ||
                !valid_view(source.source_id, false) || !valid_view(source.text, true)) {
                throw strata::compiler::ModuleLoadError(
                    "Host module loader rejected import '" + std::string(path) + "'.",
                    std::string(path)
                );
            }
            return strata::compiler::ModuleSource{
                copied_string(source.source_id),
                copied_string(source.text),
            };
        };
        strata::runtime::ActivationResult activation =
            runtime->core.compile_and_activate(
                strata::compiler::ModuleSource{
                    copied_string(config->entry_source_id),
                    copied_string(config->entry_text),
                },
                loader,
                config->generation
            );
        return finish_activation(*runtime, activation, *out_info);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Compilation or activation exhausted memory."
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Compilation or activation failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_runtime_activate_compiled_module(
    strata_runtime* const runtime,
    const strata_compiled_activation_config* const config,
    strata_activation_info* const out_info
) {
    if (runtime == nullptr) return invalid_argument();
    if (out_info == nullptr || out_info->struct_size < sizeof(strata_activation_info) ||
        config == nullptr ||
        config->struct_size < sizeof(strata_compiled_activation_config) ||
        (config->artifact.data == nullptr || config->artifact.size == 0U)) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_COMPILED_ACTIVATION_CONFIG",
            "Compiled activation requires a generation, non-empty artifact, and output structure."
        );
    }
    *out_info = strata_activation_info{
        sizeof(strata_activation_info), 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    };
    if (!runtime->core.has_application()) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.APPLICATION.NOT_CONFIGURED",
            "Configure an application before activating a compiled module."
        );
    }
    try {
        const strata::runtime::ActivationResult activation =
            runtime->core.activate_compiled(
                std::span<const std::uint8_t>(
                    config->artifact.data,
                    config->artifact.size
                ),
                config->generation
            );
        return finish_activation(*runtime, activation, *out_info);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Compiled module decoding or activation exhausted memory."
        );
    } catch (const std::exception& error) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.RUNTIME.INVALID_COMPILED_ARTIFACT",
            error.what()
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Compiled module activation failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_runtime_read_material_declarations(
    strata_runtime* const runtime,
    const strata_string_view backend,
    strata_material_declaration* const out_declarations,
    const size_t capacity,
    size_t* const out_count
) {
    if (runtime == nullptr || out_count == nullptr) return invalid_argument();
    if (!runtime->core.has_application()) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_NOT_FOUND,
            "STRATA.APPLICATION.NOT_CONFIGURED",
            "The runtime has no configured application."
        );
    }
    if (!valid_view(backend, false)) return invalid_argument();
    const std::string_view backend_id(backend.data, backend.size);
    try {
        const strata::compiler::SchemaRegistry& schemas =
            runtime->core.application().bundle()->schema_registry();
        std::size_t written = 0U;
        std::size_t declared = 0U;
        for (const std::string& id : schemas.material_ids()) {
            const strata::compiler::MaterialSchema* schema = schemas.material(id);
            if (schema == nullptr || schema->shaders.empty()) continue;
            ++declared;
            if (out_declarations == nullptr || written >= capacity) continue;
            const auto source = std::ranges::find(
                schema->shaders, backend_id, &std::pair<std::string, std::string>::first
            );
            out_declarations[written] = strata_material_declaration{
                sizeof(strata_material_declaration),
                strata_string_view{schema->id.data(), schema->id.size()},
                strata_string_view{schema->blend_mode.data(), schema->blend_mode.size()},
                strata_string_view{schema->fallback.data(), schema->fallback.size()},
                source == schema->shaders.end()
                    ? strata_string_view{nullptr, 0U}
                    : strata_string_view{source->second.data(), source->second.size()},
            };
            ++written;
        }
        *out_count = declared;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Reading material declarations failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_runtime_read_active_unit_json(
    strata_runtime* const runtime,
    const strata_value_json_sink* const sink
) {
    if (runtime == nullptr) return invalid_argument();
    if (sink == nullptr || sink->struct_size < sizeof(strata_value_json_sink) || sink->emit == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_VALUE_SINK",
            "Reading an active unit requires a complete JSON sink."
        );
    }
    if (!runtime->core.has_application() || runtime->core.application().active_unit() == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_NOT_FOUND,
            "STRATA.APPLICATION.NO_ACTIVE_UNIT",
            "The runtime has no active application unit."
        );
    }
    try {
        const std::string encoded = strata::data::encode_canonical_json(
            runtime->core.application().active_unit()->portable_ir()
        );
        sink->emit(sink->user_data, strata_string_view{encoded.data(), encoded.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Encoding the active unit exhausted memory."
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Active-unit encoding or its JSON sink failed inside the C ABI boundary."
        );
    }
}

strata_result strata_runtime_read_source_map_entry_json(
    strata_runtime* const runtime,
    const strata_string_view compiled_path,
    const strata_value_json_sink* const sink
) {
    if (runtime == nullptr) return invalid_argument();
    if (!valid_view(compiled_path, false) || !valid_json_sink(sink)) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_SOURCE_MAP_QUERY",
            "Source-map path lookup requires a non-empty compiled path and complete JSON sink."
        );
    }
    if (!runtime->core.has_application() || runtime->core.application().active_unit() == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_NOT_FOUND,
            "STRATA.APPLICATION.NO_ACTIVE_UNIT",
            "The runtime has no active application unit."
        );
    }
    try {
        const strata::compiler::CompiledSourceMapEntry* entry =
            runtime->core.application().active_unit()->source_map_entry(
                std::string_view(compiled_path.data, compiled_path.size)
            );
        if (entry == nullptr) {
            return runtime_failure(
                *runtime,
                STRATA_STATUS_NOT_FOUND,
                "STRATA.SOURCE_MAP.PATH_NOT_FOUND",
                "The active source map does not contain the requested compiled path."
            );
        }
        const std::string encoded = strata::data::encode_canonical_json(
            strata::compiler::source_map_entry_json(*entry)
        );
        sink->emit(sink->user_data, strata_string_view{encoded.data(), encoded.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Source-map lookup exhausted memory."
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Source-map lookup or its JSON sink failed inside the C ABI boundary."
        );
    }
}

strata_result strata_runtime_read_source_map_entries_at_json(
    strata_runtime* const runtime,
    const strata_string_view source_id,
    const std::uint32_t line,
    const std::uint32_t column,
    const strata_value_json_sink* const sink
) {
    if (runtime == nullptr) return invalid_argument();
    if (!valid_view(source_id, false) || line == 0U || column == 0U || !valid_json_sink(sink)) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_SOURCE_MAP_QUERY",
            "Source-map position lookup requires a source id, one-based line/column, and complete JSON sink."
        );
    }
    if (!runtime->core.has_application() || runtime->core.application().active_unit() == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_NOT_FOUND,
            "STRATA.APPLICATION.NO_ACTIVE_UNIT",
            "The runtime has no active application unit."
        );
    }
    try {
        const auto entries = runtime->core.application().active_unit()->source_map_entries_at(
            std::string_view(source_id.data, source_id.size), line, column
        );
        strata::data::JsonValue::Array values;
        values.reserve(entries.size());
        for (const strata::compiler::CompiledSourceMapEntry* entry : entries) {
            values.push_back(strata::compiler::source_map_entry_json(*entry));
        }
        const std::string encoded = strata::data::encode_canonical_json(
            strata::data::JsonValue(std::move(values))
        );
        sink->emit(sink->user_data, strata_string_view{encoded.data(), encoded.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Source-map lookup exhausted memory."
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Source-map lookup or its JSON sink failed inside the C ABI boundary."
        );
    }
}

strata_result strata_runtime_register_action_handler(
    strata_runtime* const runtime,
    const strata_action_handler_config* const config,
    strata_action_registration** const out_registration
) {
    if (out_registration != nullptr) *out_registration = nullptr;
    if (runtime == nullptr) return invalid_argument();
    if (out_registration == nullptr || config == nullptr ||
        config->struct_size < sizeof(strata_action_handler_config) ||
        !valid_view(config->action_id, false) || !valid_view(config->owner, false) ||
        config->invoke == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_ACTION_HANDLER",
            "Action registration requires an action id, owner, callback, and output handle."
        );
    }
    if (!runtime->core.has_application()) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.APPLICATION.NOT_CONFIGURED",
            "Configure an application before registering action handlers."
        );
    }
    try {
        const std::string action_id = copied_string(config->action_id);
        const auto contract = runtime->core.application().bundle()->action_registry().contract(action_id);
        if (contract == nullptr) {
            return runtime_failure(
                *runtime,
                STRATA_STATUS_NOT_FOUND,
                "STRATA.ACTION.CONTRACT_NOT_FOUND",
                "The requested action contract is not declared by the application."
            );
        }
        const strata_action_handler_fn invoke = config->invoke;
        void* const user_data = config->user_data;
        strata::runtime::ActionRegistration registration =
            runtime->core.application().actions().register_handler(
                contract,
                copied_string(config->owner),
                [invoke, user_data](const strata::runtime::ActionContext& context) {
                    const std::string payload = strata::data::encode_canonical_json(
                        strata::runtime::value_to_json(context.action.payload)
                    );
                    const std::string event_value = strata::data::encode_canonical_json(
                        strata::runtime::value_to_json(context.event.value)
                    );
                    const strata_string_view source_key = context.event.source_key.has_value()
                                                              ? strata_string_view{
                                                                    context.event.source_key->data(),
                                                                    context.event.source_key->size(),
                                                                }
                                                              : strata_string_view{nullptr, 0U};
                    const strata_action_call call{
                        sizeof(strata_action_call),
                        strata_string_view{context.action.id().data(), context.action.id().size()},
                        strata_string_view{payload.data(), payload.size()},
                        strata_string_view{context.event.kind.data(), context.event.kind.size()},
                        source_key,
                        strata_string_view{event_value.data(), event_value.size()},
                    };
                    const strata_action_handler_result response = invoke(user_data, &call);
                    if (response == STRATA_ACTION_HANDLER_HANDLED) {
                        return strata::runtime::ActionHandlerResult::handled;
                    }
                    if (response == STRATA_ACTION_HANDLER_FORWARDED) {
                        return strata::runtime::ActionHandlerResult::forwarded;
                    }
                    if (response == STRATA_ACTION_HANDLER_IGNORED) {
                        return strata::runtime::ActionHandlerResult::ignored;
                    }
                    throw std::runtime_error("foreign action handler returned an invalid result");
                }
            );
        const strata::core::HostAllocator allocator = runtime->core.allocator();
        void* const storage = allocator.allocate(
            sizeof(strata_action_registration),
            alignof(strata_action_registration)
        );
        if (storage == nullptr) {
            return runtime_failure(
                *runtime,
                STRATA_STATUS_OUT_OF_MEMORY,
                "STRATA.CORE.OUT_OF_MEMORY",
                "The action registration handle could not be allocated."
            );
        }
        *out_registration = std::construct_at(
            static_cast<strata_action_registration*>(storage),
            allocator,
            std::move(registration)
        );
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Action handler registration exhausted memory."
        );
    } catch (const std::invalid_argument& error) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ACTION.INVALID_HANDLER",
            error.what()
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Action handler registration failed inside the C ABI exception boundary."
        );
    }
}

void strata_action_registration_release(strata_action_registration* const registration) {
    if (registration == nullptr) return;
    const strata::core::HostAllocator allocator = registration->allocator;
    std::destroy_at(registration);
    allocator.deallocate(
        registration,
        sizeof(strata_action_registration),
        alignof(strata_action_registration)
    );
}

strata_result strata_runtime_dispatch_action_json(
    strata_runtime* const runtime,
    const strata_action_dispatch_config* const config,
    strata_action_dispatch_info* const out_info
) {
    if (runtime == nullptr) return invalid_argument();
    if (!valid_action_dispatch_config(config) || out_info == nullptr ||
        out_info->struct_size < sizeof(strata_action_dispatch_info)) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_ACTION_DISPATCH",
            "Action dispatch requires an action id, payload/event JSON, event kind, and output structure."
        );
    }
    *out_info = strata_action_dispatch_info{
        sizeof(strata_action_dispatch_info),
        STRATA_ACTION_DISPATCH_FAILED,
        0U,
        0U,
    };
    if (!runtime->core.has_application()) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.APPLICATION.NOT_CONFIGURED",
            "Configure an application before dispatching actions."
        );
    }
    try {
        const std::string action_id = copied_string(config->action_id);
        const bool dynamic = dynamic_action_dispatch(*config);
        const auto contract = runtime->core.application().bundle()->action_registry().contract(action_id);
        if (!dynamic && contract == nullptr) {
            return runtime_failure(
                *runtime,
                STRATA_STATUS_NOT_FOUND,
                "STRATA.ACTION.CONTRACT_NOT_FOUND",
                "The dispatched action contract is not declared by the application."
            );
        }
        const strata::runtime::Action action = dynamic
            ? strata::runtime::dynamic_action(
                  action_id,
                  decoded_value(config->payload_json, true)
              )
            : strata::runtime::Action(
                  contract,
                  decoded_value(config->payload_json, true)
              );
        const strata::runtime::ActionEvent event{
            copied_string(config->event_kind),
            config->source_key.size != 0U
                ? std::optional<std::string>(copied_string(config->source_key))
                : std::nullopt,
            decoded_value(config->event_value_json, true),
        };
        const strata::runtime::ActionDispatchOutcome outcome =
            runtime->core.application().dispatch(
                event,
                action,
                config->state_scope.size != 0U
                    ? std::string_view(config->state_scope.data, config->state_scope.size)
                    : std::string_view{}
            );
        *out_info = strata_action_dispatch_info{
            sizeof(strata_action_dispatch_info),
            dispatch_status(outcome.status),
            0U,
            static_cast<std::uint64_t>(outcome.handler_owners.size()),
        };
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const strata::data::JsonError&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ACTION.INVALID_JSON",
            "Action payload or event value is not valid strict JSON."
        );
    } catch (const std::invalid_argument& error) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ACTION.INVALID_PAYLOAD",
            error.what()
        );
    } catch (const std::bad_alloc&) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Action dispatch exhausted memory."
        );
    } catch (...) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Action dispatch failed inside the C ABI exception boundary."
        );
    }
}
