#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>

#include <strata/strata.h>

/*
 * Struct-size versioning for every caller-provided ABI struct. A struct only ever grows at its
 * end, and `struct_size` says which prefix the caller's headers had. The library accepts any
 * prefix at least as large as the oldest layout it still supports, reads only the bytes the caller
 * provided, and treats every newer field as zero (its documented default). Output structs are
 * written only up to the caller's declared size.
 */
namespace strata::abi_detail {

/**
 * Size of the oldest layout of `T` this library accepts. A struct that has never grown accepts
 * only its full size; when a field is appended, specialize this with the previous size.
 */
template <typename T> inline constexpr std::size_t oldest_struct_size = sizeof(T);

template <>
inline constexpr std::size_t oldest_struct_size<strata_surface_config> =
    offsetof(strata_surface_config, extensions);
template <>
inline constexpr std::size_t oldest_struct_size<strata_action_dispatch_config> =
    offsetof(strata_action_dispatch_config, dynamic);
template <>
inline constexpr std::size_t oldest_struct_size<strata_surface_extension_bundle> =
    STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_1_SIZE;
template <>
inline constexpr std::size_t oldest_struct_size<strata_widget_extension> =
    STRATA_WIDGET_EXTENSION_VERSION_1_SIZE;
template <>
inline constexpr std::size_t oldest_struct_size<strata_behavior_extension> =
    STRATA_BEHAVIOR_EXTENSION_VERSION_1_SIZE;
template <>
inline constexpr std::size_t oldest_struct_size<strata_surface_environment> =
    STRATA_SURFACE_ENVIRONMENT_VERSION_1_SIZE;

/** True when `value` carries at least the oldest accepted prefix of `T`. */
template <typename T> [[nodiscard]] bool has_struct(const T* const value) noexcept {
    return value != nullptr && value->struct_size >= oldest_struct_size<T>;
}

/** True when the caller's prefix of `value` includes the member ending at `member_end`. */
template <typename T>
[[nodiscard]] bool provides(const T& value, const std::size_t member_end) noexcept {
    return value.struct_size >= member_end;
}

/**
 * Copies the caller's prefix of `value`; fields newer than the caller's layout are zero. The
 * result's `struct_size` is the number of bytes the caller provided, capped at the current size.
 */
template <typename T> [[nodiscard]] std::optional<T> read_struct(const T* const value) noexcept {
    if (!has_struct(value))
        return std::nullopt;
    T result{};
    const std::size_t size = std::min(value->struct_size, sizeof(T));
    std::memcpy(&result, value, size);
    result.struct_size = size;
    return result;
}

/**
 * Reads element `index` of a caller array of versioned structs. The array is strided by the
 * caller's struct size, which the first element declares and every element must repeat.
 */
template <typename T>
[[nodiscard]] std::optional<T> read_struct_element(const T* const array,
                                                   const std::size_t index) noexcept {
    if (array == nullptr)
        return std::nullopt;
    std::size_t stride = 0U;
    std::memcpy(&stride, reinterpret_cast<const std::byte*>(array) + offsetof(T, struct_size),
                sizeof(stride));
    if (stride < oldest_struct_size<T>)
        return std::nullopt;
    const std::byte* const element = reinterpret_cast<const std::byte*>(array) + index * stride;
    std::size_t element_size = 0U;
    std::memcpy(&element_size, element + offsetof(T, struct_size), sizeof(element_size));
    if (element_size != stride)
        return std::nullopt;
    T result{};
    const std::size_t size = std::min(stride, sizeof(T));
    std::memcpy(&result, element, size);
    result.struct_size = size;
    return result;
}

/**
 * Copies a caller's surface configuration, of any layout. Its environment is embedded rather than
 * pointed to, so a caller built against an older, shorter environment placed every later field
 * that much earlier: the environment is read at the size it declares (newer environment fields
 * read as zero), and the rest of the configuration from where that caller's layout put it.
 */
[[nodiscard]] inline std::optional<strata_surface_config>
read_surface_config(const strata_surface_config* const value) noexcept {
    constexpr std::size_t environment_offset = offsetof(strata_surface_config, environment);
    constexpr std::size_t environment_size = sizeof(strata_surface_environment);
    if (value == nullptr || value->struct_size < environment_offset + sizeof(std::size_t))
        return std::nullopt;
    const auto* const bytes = reinterpret_cast<const std::byte*>(value);
    std::size_t declared = 0U;
    std::memcpy(&declared, bytes + environment_offset, sizeof(declared));
    if (declared >= environment_size)
        return read_struct(value);
    const std::size_t caller_tail = environment_offset + declared;
    if (declared < oldest_struct_size<strata_surface_environment> ||
        value->struct_size < caller_tail ||
        value->struct_size + (environment_size - declared) <
            oldest_struct_size<strata_surface_config>) {
        return std::nullopt;
    }
    strata_surface_config result{};
    auto* const target = reinterpret_cast<std::byte*>(&result);
    std::memcpy(target, bytes, caller_tail);
    const std::size_t tail =
        std::min(value->struct_size - caller_tail,
                 sizeof(strata_surface_config) - environment_offset - environment_size);
    std::memcpy(target + environment_offset + environment_size, bytes + caller_tail, tail);
    result.struct_size = environment_offset + environment_size + tail;
    return result;
}

/** Writes `value` into a caller output struct, never past the size the caller declared. */
template <typename T> [[nodiscard]] bool write_struct(T* const output, T value) noexcept {
    if (!has_struct(output))
        return false;
    const std::size_t size = std::min(output->struct_size, sizeof(T));
    value.struct_size = size;
    std::memcpy(output, &value, size);
    return true;
}

} // namespace strata::abi_detail
