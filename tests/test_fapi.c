#include "ctrace/trace.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

typedef struct {
    uint16_t cce_index;
    uint16_t rnti;
} fapi_cce_t;

// 3rd-party vendor struct with large fixed array
typedef struct {
    uint16_t num_cce;
    fapi_cce_t cce_list[64];
    uint8_t rb_bitmap[36]; // 36-byte fixed array
} fapi_dl_tti_req_t;

typedef struct {
    uint32_t tti;
    uint16_t mcs;
} fapi_dlsch_pdu_t;

typedef struct {
    uint32_t freq_domain;
    uint16_t aggregation_level;
} fapi_pdcch_pdu_t;

// 3rd-party vendor struct with union
typedef struct {
    uint16_t pdu_type;
    uint16_t pdu_size;
    union {
        fapi_dlsch_pdu_t dlsch_pdu;
        fapi_pdcch_pdu_t pdcch_pdu;
    } u;
} fapi_pdu_t;

/* Bug 1: fixed array inside an element of a counted struct array */
typedef struct { uint8_t size; uint8_t bits[16]; } fapi_dci_t;
typedef struct { uint16_t n; fapi_dci_t list[10]; } fapi_dci_ind_t;
CTRACE_BIND_COUNT(fapi_dci_ind_t, list, n);

/* Bug 2: anonymous discriminated union + Bug 3: multiple tags to same union member */
typedef struct { uint16_t a; } fapi_prach_t;
typedef struct { uint32_t b; uint16_t c; } fapi_pucch_t;
typedef struct {
    int type;
    union { fapi_prach_t prach; fapi_pucch_t pucch; };
} fapi_anon_pdu_t;
CTRACE_BIND_UNION(fapi_anon_pdu_t, type, 1, prach);
CTRACE_BIND_UNION(fapi_anon_pdu_t, type, 2, pucch);
CTRACE_BIND_UNION(fapi_anon_pdu_t, type, 3, pucch); // Tag 3 also maps to pucch

/* Bug 4: count-bound byte payload decoded as hex string */
typedef struct {
    uint32_t payload_len;
    uint8_t *payload;
} fapi_rx_data_t;
CTRACE_BIND_COUNT(fapi_rx_data_t, payload, payload_len);

/* Bug 5: union-typed field with no binding (e.g. pthread_mutex_t) */
typedef struct {
    int n;
    pthread_mutex_t m;
} fapi_with_mutex_t;

/* Bug 6: union variant holding a count-bound array */
typedef struct {
    uint16_t rnti;
    uint8_t n_cand;
    uint16_t cce[64];
    uint8_t opts[2];
} fapi_dci_variant_t;
typedef struct { int ta_val; } fapi_ta_variant_t;
typedef struct {
    uint8_t type;
    union {
        fapi_dci_variant_t dci;
        fapi_ta_variant_t ta;
    };
} fapi_dyn_pdu_t;
CTRACE_BIND_COUNT(fapi_dci_variant_t, cce, n_cand);
CTRACE_BIND_UNION(fapi_dyn_pdu_t, type, 1, dci);
CTRACE_BIND_UNION(fapi_dyn_pdu_t, type, 8, ta);

/* Negative tag value selecting an error variant */
typedef struct { int32_t code; } fapi_err_t;
typedef struct { uint16_t len; } fapi_ok_t;
typedef struct {
    int status;
    union { fapi_err_t err; fapi_ok_t ok; } r;
} fapi_result_t;
CTRACE_BIND_UNION(fapi_result_t, status, -1, err);
CTRACE_BIND_UNION(fapi_result_t, status, 0, ok);

/* Tag value above INT32_MAX in an unsigned tag field */
typedef struct { uint8_t a; } fapi_big_a_t;
typedef struct { uint32_t b; } fapi_big_b_t;
typedef struct {
    uint32_t kind;
    union { fapi_big_a_t a; fapi_big_b_t b; } v;
} fapi_big_tag_t;
CTRACE_BIND_UNION(fapi_big_tag_t, kind, 0xFFFFFFFFu, a);
CTRACE_BIND_UNION(fapi_big_tag_t, kind, 7, b);

/* Two unions in one struct, each with its own tag field */
typedef struct { uint16_t a1; } fapi_two_a_t;
typedef struct { uint32_t b1; } fapi_two_b_t;
typedef struct { uint8_t c1; } fapi_two_c_t;
typedef struct { int16_t d1; } fapi_two_d_t;
typedef struct {
    uint8_t t1;
    union { fapi_two_a_t a; fapi_two_b_t b; } u1;
    uint8_t t2;
    union { fapi_two_c_t c; fapi_two_d_t d; } u2;
} fapi_two_unions_t;
CTRACE_BIND_UNION(fapi_two_unions_t, t1, 1, a);
CTRACE_BIND_UNION(fapi_two_unions_t, t1, 2, b);
CTRACE_BIND_UNION(fapi_two_unions_t, t2, 5, c);
CTRACE_BIND_UNION(fapi_two_unions_t, t2, 6, d);

/* Union variant holding a nested struct */
typedef struct { uint16_t x; uint16_t y; } fapi_hdr_t;
typedef struct { fapi_hdr_t hdr; uint32_t body; } fapi_nested_variant_t;
typedef struct {
    uint8_t sel;
    union { fapi_nested_variant_t nv; uint32_t plain; } u;
} fapi_nested_pdu_t;
CTRACE_BIND_UNION(fapi_nested_pdu_t, sel, 1, nv);
CTRACE_BIND_UNION(fapi_nested_pdu_t, sel, 2, plain);

/* Bare union passed by pointer */
typedef union { uint32_t word; uint8_t bytes[4]; } fapi_word_t;

/* Counted struct array whose element has two bools */
typedef struct { bool on; bool err; uint8_t lvl; } fapi_flags_t;
typedef struct {
    uint8_t n;
    fapi_flags_t flags[4];
} fapi_flags_list_t;
CTRACE_BIND_COUNT(fapi_flags_list_t, flags, n);

/* C11 anonymous struct member */
typedef struct {
    int id;
    struct { uint16_t x; uint16_t y; };
} fapi_anon_struct_t;

// Out-of-line bindings (without modifying 3rd-party vendor headers)
CTRACE_BIND_COUNT(fapi_dl_tti_req_t, cce_list, num_cce);
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 0, dlsch_pdu);
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 1, pdcch_pdu);
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 2, dlsch_pdu); // Tag 2 also maps to dlsch_pdu

int main(void) {
    ctrace_init("test_fapi.bin");

    fapi_dl_tti_req_t req;
    memset(&req, 0, sizeof(req));
    req.num_cce = 2; // only 2 of 64 should be serialized
    req.cce_list[0].cce_index = 10;
    req.cce_list[0].rnti = 0x1234;
    req.cce_list[1].cce_index = 20;
    req.cce_list[1].rnti = 0x5678;
    for (int i = 0; i < 36; ++i) {
        req.rb_bitmap[i] = (uint8_t)(i + 1);
    }

    TRACE_EVENT(fapi_tti, req);

    fapi_pdu_t pdu1;
    memset(&pdu1, 0, sizeof(pdu1));
    pdu1.pdu_type = 0; // dlsch_pdu
    pdu1.pdu_size = sizeof(fapi_dlsch_pdu_t);
    pdu1.u.dlsch_pdu.tti = 1001;
    pdu1.u.dlsch_pdu.mcs = 28;

    TRACE_EVENT(fapi_pdu_event, pdu1);

    fapi_pdu_t pdu2;
    memset(&pdu2, 0, sizeof(pdu2));
    pdu2.pdu_type = 1; // pdcch_pdu
    pdu2.pdu_size = sizeof(fapi_pdcch_pdu_t);
    pdu2.u.pdcch_pdu.freq_domain = 0xABCD;
    pdu2.u.pdcch_pdu.aggregation_level = 4;

    TRACE_EVENT(fapi_pdu_event, pdu2);

    // Test multiple tag mapping to dlsch_pdu (tag 2)
    fapi_pdu_t pdu3;
    memset(&pdu3, 0, sizeof(pdu3));
    pdu3.pdu_type = 2; // also dlsch_pdu
    pdu3.pdu_size = sizeof(fapi_dlsch_pdu_t);
    pdu3.u.dlsch_pdu.tti = 2002;
    pdu3.u.dlsch_pdu.mcs = 14;

    TRACE_EVENT(fapi_pdu_event, pdu3);

    // Test fixed array inside element of counted struct array
    fapi_dci_ind_t dci_ind;
    memset(&dci_ind, 0, sizeof(dci_ind));
    dci_ind.n = 1;
    dci_ind.list[0].size = 5;
    dci_ind.list[0].bits[0] = 0xAA;
    dci_ind.list[0].bits[1] = 0xBB;
    dci_ind.list[0].bits[2] = 0xCC;

    TRACE_EVENT(fapi_dci_event, dci_ind);

    // Test anonymous discriminated union
    fapi_anon_pdu_t anon1;
    memset(&anon1, 0, sizeof(anon1));
    anon1.type = 2; // pucch
    anon1.pucch.b = 8888;
    anon1.pucch.c = 99;

    TRACE_EVENT(fapi_anon_event, anon1);

    // Test count-bound byte payload (should decode as hex string)
    uint8_t raw_payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    fapi_rx_data_t rx_data;
    rx_data.payload_len = 4;
    rx_data.payload = raw_payload;

    TRACE_EVENT(fapi_rx_event, rx_data);

    // Test unbound union field (pthread_mutex_t)
    fapi_with_mutex_t wm;
    memset(&wm, 0, sizeof(wm));
    wm.n = 42;
    TRACE_EVENT(fapi_raw_event, wm);

    // Test union variant holding a count-bound array
    fapi_dyn_pdu_t dyn1;
    memset(&dyn1, 0, sizeof(dyn1));
    dyn1.type = 1;
    dyn1.dci.rnti = 0x1234;
    dyn1.dci.n_cand = 2;
    dyn1.dci.cce[0] = 5;
    dyn1.dci.cce[1] = 9;
    dyn1.dci.opts[0] = 7;
    dyn1.dci.opts[1] = 8;
    TRACE_EVENT(fapi_dyn_pdu_event, dyn1);

    fapi_dyn_pdu_t dyn2;
    memset(&dyn2, 0, sizeof(dyn2));
    dyn2.type = 8;
    dyn2.ta.ta_val = 31;
    TRACE_EVENT(fapi_dyn_pdu_event, dyn2);

    // Unmatched tag: the union falls back to its raw bytes
    fapi_pdu_t pdu4;
    memset(&pdu4, 0, sizeof(pdu4));
    pdu4.pdu_type = 99;
    pdu4.u.dlsch_pdu.tti = 0x11223344;
    TRACE_EVENT(fapi_pdu_event, pdu4);

    // Negative tag value
    fapi_result_t res;
    memset(&res, 0, sizeof(res));
    res.status = -1;
    res.r.err.code = -22;
    TRACE_EVENT(fapi_result_event, res);

    // Tag value above INT32_MAX
    fapi_big_tag_t big;
    memset(&big, 0, sizeof(big));
    big.kind = 0xFFFFFFFFu;
    big.v.a.a = 200;
    TRACE_EVENT(fapi_big_tag_event, big);

    // Two unions with distinct tags in one struct
    fapi_two_unions_t two;
    memset(&two, 0, sizeof(two));
    two.t1 = 2;
    two.u1.b.b1 = 70000;
    two.t2 = 5;
    two.u2.c.c1 = 9;
    TRACE_EVENT(fapi_two_unions_event, two);

    // Nested struct inside a union variant
    fapi_nested_pdu_t nested;
    memset(&nested, 0, sizeof(nested));
    nested.sel = 1;
    nested.u.nv.hdr.x = 3;
    nested.u.nv.hdr.y = 4;
    nested.u.nv.body = 500;
    TRACE_EVENT(fapi_nested_event, nested);

    // Bare union by pointer, and a NULL union pointer
    fapi_word_t word;
    word.word = 0x04030201;
    fapi_word_t *wp = &word;
    fapi_word_t *wnull = NULL;
    TRACE_EVENT(fapi_word_event, wp);
    TRACE_EVENT(fapi_word_event, wnull);

    // Counted struct array with two bool fields per element
    fapi_flags_list_t fl;
    memset(&fl, 0, sizeof(fl));
    fl.n = 2;
    fl.flags[0].on = true;  fl.flags[0].err = false; fl.flags[0].lvl = 1;
    fl.flags[1].on = false; fl.flags[1].err = true;  fl.flags[1].lvl = 2;
    TRACE_EVENT(fapi_flags_event, fl);

    // Fixed-size array passed directly
    uint16_t samples[6] = { 1, 2, 3, 4, 5, 6 };
    TRACE_EVENT(fapi_samples_event, samples);

    // Anonymous struct member
    fapi_anon_struct_t as;
    memset(&as, 0, sizeof(as));
    as.id = 12;
    as.x = 7;
    as.y = 9;
    TRACE_EVENT(fapi_anon_struct_event, as);

    ctrace_flush();
    ctrace_shutdown();
    return 0;
}
