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

#define _CTRACE_GLUE2(a, b) a##b
#define _CTRACE_GLUE(a, b) _CTRACE_GLUE2(a, b)

#if defined(CTRACE_AST_SCAN) && (defined(__clang__) || defined(__GNUC__))
#define CTRACE_COUNTED_BY(field) __attribute__((annotate("ctrace:counted_by:" #field)))

// Out-of-line count binding for 3rd-party struct arrays/pointers (no header changes needed):
// Usage in .c file: CTRACE_BIND_COUNT(fapi_dl_config_req_t, pdus, num_pdus);
#define _CTRACE_BIND_COUNT_IMPL(type, array_field, count_field, uid) \
    static const type *_CTRACE_GLUE(__ctrace_bind_count_, uid) \
    __attribute__((annotate("ctrace:bind_count:" #type ":" #array_field ":" #count_field))) \
    __attribute__((unused)) = 0

#define _CTRACE_BIND_COUNT_HELPER(type, array_field, count_field, ctr) \
    _CTRACE_BIND_COUNT_IMPL(type, array_field, count_field, ctr)

#define CTRACE_BIND_COUNT(type, array_field, count_field) \
    _CTRACE_BIND_COUNT_HELPER(type, array_field, count_field, __COUNTER__)

// Out-of-line discriminated union tag binding for 3rd-party structs (no header changes needed):
// Usage in .c file: CTRACE_BIND_UNION(fapi_dl_config_pdu_t, pdu_type, FAPI_DL_DCI_PDU_TYPE, dci_pdu);
#define _CTRACE_BIND_UNION_IMPL(type, tag_field, tag_val, member_name, uid) \
    static const type *_CTRACE_GLUE(__ctrace_bu_ptr_, uid) \
    __attribute__((annotate("ctrace:bind_union:" #type ":" #tag_field ":" #tag_val ":" #member_name))) \
    __attribute__((unused)) = 0; \
    static const long long _CTRACE_GLUE(__ctrace_bu_val_, uid) \
    __attribute__((annotate("ctrace:bind_union_val:" #type ":" #tag_field ":" #tag_val ":" #member_name))) \
    __attribute__((unused)) = (long long)(tag_val)

#define _CTRACE_BIND_UNION_HELPER(type, tag_field, tag_val, member_name, ctr) \
    _CTRACE_BIND_UNION_IMPL(type, tag_field, tag_val, member_name, ctr)

#define CTRACE_BIND_UNION(type, tag_field, tag_val, member_name) \
    _CTRACE_BIND_UNION_HELPER(type, tag_field, tag_val, member_name, __COUNTER__)
#else
#define CTRACE_COUNTED_BY(field)

#define _CTRACE_BIND_COUNT_DEF_IMPL(uid) \
    typedef int _CTRACE_GLUE(__ctrace_bind_count_, uid) __attribute__((unused))
#define _CTRACE_BIND_COUNT_DEF_HELPER(ctr) _CTRACE_BIND_COUNT_DEF_IMPL(ctr)
#define CTRACE_BIND_COUNT(type, array_field, count_field) \
    _CTRACE_BIND_COUNT_DEF_HELPER(__COUNTER__)

#define _CTRACE_BIND_UNION_DEF_IMPL(uid) \
    typedef int _CTRACE_GLUE(__ctrace_bind_union_, uid) __attribute__((unused))
#define _CTRACE_BIND_UNION_DEF_HELPER(ctr) _CTRACE_BIND_UNION_DEF_IMPL(ctr)
#define CTRACE_BIND_UNION(type, tag_field, tag_val, member_name) \
    _CTRACE_BIND_UNION_DEF_HELPER(__COUNTER__)
#endif

#ifdef __cplusplus
}
#endif
