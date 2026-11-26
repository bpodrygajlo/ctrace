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

// Out-of-line count binding for 3rd-party struct arrays/pointers (no header changes needed):
// Usage in .c file: CTRACE_BIND_COUNT(fapi_dl_config_req_t, pdus, num_pdus);
#define CTRACE_BIND_COUNT(type, array_field, count_field) \
    static const type *__ctrace_bind_count_##type##_##array_field \
    __attribute__((annotate("ctrace:bind_count:" #type ":" #array_field ":" #count_field))) \
    __attribute__((unused)) = 0

// Out-of-line discriminated union tag binding for 3rd-party structs (no header changes needed):
// Usage in .c file: CTRACE_BIND_UNION(fapi_dl_config_pdu_t, pdu_type, FAPI_DL_DCI_PDU_TYPE, dci_pdu);
#define CTRACE_BIND_UNION(type, tag_field, tag_val, member_name) \
    static const type *__ctrace_bu_ptr_##type##_##member_name \
    __attribute__((annotate("ctrace:bind_union:" #type ":" #tag_field ":" #tag_val ":" #member_name))) \
    __attribute__((unused)) = 0; \
    static const long long __ctrace_bu_val_##type##_##member_name \
    __attribute__((annotate("ctrace:bind_union_val:" #type ":" #tag_field ":" #member_name))) \
    __attribute__((unused)) = (long long)(tag_val)
#else
#define CTRACE_COUNTED_BY(field)
#define CTRACE_BIND_COUNT(type, array_field, count_field) \
    typedef int __ctrace_bind_count_##type##_##array_field __attribute__((unused))
#define CTRACE_BIND_UNION(type, tag_field, tag_val, member_name) \
    typedef int __ctrace_bind_union_##type##_##member_name __attribute__((unused))
#endif

#ifdef __cplusplus
}
#endif
