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

// Out-of-line bindings (without modifying 3rd-party vendor headers)
CTRACE_BIND_COUNT(fapi_dl_tti_req_t, cce_list, num_cce);
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 0, dlsch_pdu);
CTRACE_BIND_UNION(fapi_pdu_t, pdu_type, 1, pdcch_pdu);

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

    ctrace_flush();
    ctrace_shutdown();
    return 0;
}
