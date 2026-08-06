#ifndef STRATA_ADAPTER_H
#define STRATA_ADAPTER_H

#include <strata/strata.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed failure detail valid until the next adapter call on the same thread. */
typedef struct strata_adapter_result {
    strata_status status;
    uint32_t reserved;
    strata_string_view message;
} strata_adapter_result;

#ifdef __cplusplus
}
#endif

#endif
