#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const void *ptr;
    size_t len;
} ctrace_bytes_t;

#define TRACE_BYTES(p, l) ((ctrace_bytes_t){ .ptr = (const void *)(p), .len = (size_t)(l) })

#if defined(__clang__) || defined(__GNUC__)
#define CTRACE_COUNTED_BY(field) __attribute__((annotate("ctrace:counted_by:" #field)))
#else
#define CTRACE_COUNTED_BY(field)
#endif

#ifdef __cplusplus
}
#endif
