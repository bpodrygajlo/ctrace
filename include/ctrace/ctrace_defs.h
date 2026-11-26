#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ctrace requires a little-endian target architecture"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const void *ptr;
    size_t len;
} ctrace_bytes_t;

static inline ctrace_bytes_t ctrace_make_bytes(const void *p, size_t l) {
    ctrace_bytes_t b;
    b.ptr = p;
    b.len = l;
    return b;
}

#define TRACE_BYTES(p, l) ctrace_make_bytes((const void *)(p), (size_t)(l))

#if defined(CTRACE_AST_SCAN) && (defined(__clang__) || defined(__GNUC__))
#define CTRACE_COUNTED_BY(field) __attribute__((annotate("ctrace:counted_by:" #field)))
#else
#define CTRACE_COUNTED_BY(field)
#endif

#ifdef __cplusplus
}
#endif
