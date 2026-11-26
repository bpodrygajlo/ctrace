#include "ctrace/trace.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

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

    ctrace_flush();
    ctrace_shutdown();
    return 0;
}
