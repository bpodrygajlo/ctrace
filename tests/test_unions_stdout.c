#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "ctrace/trace.h"

/* Named discriminated union */
typedef struct { uint32_t tti; uint16_t mcs; } dl_pdu_t;
typedef struct { uint32_t freq; uint16_t agg; } pdcch_pdu_t;
typedef struct {
    uint16_t pdu_type;
    union { dl_pdu_t dl; pdcch_pdu_t pdcch; } u;
} pdu_t;
CTRACE_BIND_UNION(pdu_t, pdu_type, 0, dl);
CTRACE_BIND_UNION(pdu_t, pdu_type, 1, pdcch);

/* Anonymous discriminated union */
typedef struct { uint16_t a; } prach_t;
typedef struct { uint32_t b; uint16_t c; } pucch_t;
typedef struct {
    int type;
    union { prach_t prach; pucch_t pucch; };
} anon_pdu_t;
CTRACE_BIND_UNION(anon_pdu_t, type, 1, prach);
CTRACE_BIND_UNION(anon_pdu_t, type, 2, pucch);

/* Two unions with their own tags, scalar members */
typedef struct {
    uint8_t t1;
    union { uint16_t a; uint32_t b; } u1;
    uint8_t t2;
    union { uint8_t c; int16_t d; } u2;
} two_t;
CTRACE_BIND_UNION(two_t, t1, 1, a);
CTRACE_BIND_UNION(two_t, t1, 2, b);
CTRACE_BIND_UNION(two_t, t2, 5, c);
CTRACE_BIND_UNION(two_t, t2, 6, d);

/* Bare union, passed by pointer */
typedef union { uint32_t word; uint8_t bytes[4]; } word_t;

/* Unbound union member */
typedef struct { int n; pthread_mutex_t m; } with_mutex_t;

/* Fixed array of padded structs, and a field skipped in the header */
typedef enum { RC_OK = 0, RC_BAD = 1 } rc_t;
typedef struct { uint8_t a; rc_t rc; uint32_t b; } cell_t;
typedef struct {
    int secret CTRACE_SKIP;
    cell_t cells[6];
} cells_t;

/* Anonymous struct member */
typedef struct {
    int id;
    struct { uint16_t x; uint16_t y; };
} anon_struct_t;

static int log_has(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    char buf[4096];
    int found = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, needle)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    if (!found) {
        fprintf(stderr, "missing in log: %s\n", needle);
    }
    return found;
}

int main(void) {
    ctrace_init("test_unions_stdout.log");

    pdu_t pdu;
    memset(&pdu, 0, sizeof(pdu));
    pdu.pdu_type = 1;
    pdu.u.pdcch.freq = 0xABCD;
    pdu.u.pdcch.agg = 4;
    TRACE_EVENT(pdu_event, pdu);

    pdu_t pdu_unknown;
    memset(&pdu_unknown, 0, sizeof(pdu_unknown));
    pdu_unknown.pdu_type = 99;
    pdu_unknown.u.dl.tti = 0x11223344;
    TRACE_EVENT(pdu_event, pdu_unknown);

    anon_pdu_t anon;
    memset(&anon, 0, sizeof(anon));
    anon.type = 2;
    anon.pucch.b = 8888;
    anon.pucch.c = 99;
    TRACE_EVENT(anon_event, anon);

    two_t two;
    memset(&two, 0, sizeof(two));
    two.t1 = 2;
    two.u1.b = 70000;
    two.t2 = 5;
    two.u2.c = 9;
    TRACE_EVENT(two_event, two);

    word_t word;
    word.word = 0x04030201;
    word_t *wp = &word;
    word_t *wnull = NULL;
    TRACE_EVENT(word_event, wp);
    TRACE_EVENT(word_event, wnull);

    with_mutex_t wm;
    memset(&wm, 0, sizeof(wm));
    wm.n = 42;
    TRACE_EVENT(mutex_event, wm);

    uint16_t samples[6] = { 1, 2, 3, 4, 5, 6 };
    TRACE_EVENT(samples_event, samples);

    anon_struct_t as;
    memset(&as, 0, sizeof(as));
    as.id = 12;
    as.x = 7;
    as.y = 9;
    TRACE_EVENT(anon_struct_event, as);

    cells_t cells;
    memset(&cells, 0, sizeof(cells));
    cells.secret = 123;
    cells.cells[1].a = 7;
    cells.cells[1].rc = RC_BAD;
    cells.cells[1].b = 500;
    cells.cells[5].a = 9;
    TRACE_EVENT(cells_event, cells);

    ctrace_flush();
    ctrace_shutdown();

    const char *log = "test_unions_stdout.log";
    assert(log_has(log, "pdu_event pdu={pdu_type=1, u={pdcch={freq=43981, agg=4}}}"));
    // Field names come from the first call site of an event
    assert(log_has(log, "pdu_event pdu={pdu_type=99, u=[union_len=8,hex=4433221100000000]}"));
    assert(log_has(log, "anon_event anon={type=2, u={pucch={b=8888, c=99}}}"));
    assert(log_has(log, "two_event two={t1=2, u1={b=70000}, t2=5, u2={c=9}}"));
    assert(log_has(log, "word_event wp=[union_len=4,hex=01020304]"));
    assert(log_has(log, "word_event wp=(null)"));
    assert(log_has(log, "mutex_event wm={n=42, m=[union_len="));
    assert(log_has(log, "hex=00000000000000000000000000000000"));
    assert(log_has(log, "samples_event samples=[len=12,hex=010002000300040005000600]"));
    assert(log_has(log, "anon_struct_event as={id=12, x=7, y=9}"));
    assert(log_has(log, "cells_event cells={cells=[{a=0, rc=RC_OK, b=0}, {a=7, rc=RC_BAD, b=500}, {a=0, rc=RC_OK, b=0}, "
                        "{a=0, rc=RC_OK, b=0}, {a=0, rc=RC_OK, b=0}, {a=9, rc=RC_OK, b=0}]}"));
    assert(!log_has(log, "secret"));

    printf("test_unions_stdout passed successfully!\n");
    return 0;
}
