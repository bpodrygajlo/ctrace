#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sqlite3.h>

/* Two counted struct arrays, a discriminated union and a trailing scalar in one row */
typedef struct { uint16_t id; bool on; } SqItem;
typedef struct { uint8_t p; } SqV1;
typedef struct { uint32_t q; } SqV2;
typedef struct {
    uint16_t na;
    SqItem *a CTRACE_COUNTED_BY(na);
    uint16_t nb;
    SqItem *b CTRACE_COUNTED_BY(nb);
    uint8_t kind;
    union { SqV1 v1; SqV2 v2; } u;
    int32_t tail;
} SqBatch;
CTRACE_BIND_UNION(SqBatch, kind, 1, v1);
CTRACE_BIND_UNION(SqBatch, kind, 2, v2);

static void query_text(sqlite3 *db, const char *sql, char *out, size_t out_len) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    assert(rc == SQLITE_ROW);
    const unsigned char *txt = sqlite3_column_text(stmt, 0);
    snprintf(out, out_len, "%s", txt ? (const char *)txt : "(null)");
    sqlite3_finalize(stmt);
}

static int query_count(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    assert(rc == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int main(void) {
    // Run 1: Write trace directly to a SQLite database file
    ctrace_init("test_sqlite.db");

    int socket_fd = 77;
    size_t len = 512;
    const char *payload_data = "sqlitedata";
    ctrace_bytes_t payload = TRACE_BYTES(payload_data, 10);
    const char *url = "http://api.local/query";
    LogLevel lvl = LOG_INFO;
    bool active = true;

    for (int i = 0; i < 25; ++i) {
        TRACE_EVENT(net_rx, socket_fd + i, len, payload);
    }
    TRACE_EVENT(api_call, url, lvl, active);

    SqItem items_a[2] = { { .id = 1, .on = true }, { .id = 2, .on = false } };
    SqItem items_b[1] = { { .id = 3, .on = true } };
    SqBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.na = 2;
    batch.a = items_a;
    batch.nb = 1;
    batch.b = items_b;
    batch.kind = 2;
    batch.u.v2.q = 0xAABBCCDDu;
    batch.tail = 77;
    TRACE_EVENT(sq_batch, batch);

    ctrace_flush();
    ctrace_shutdown();

    // Offline investigation: open the saved SQLite file directly with sqlite3
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("test_sqlite.db", &db, SQLITE_OPEN_READONLY, NULL);
    assert(rc == SQLITE_OK && db != NULL);
    assert(query_count(db, "SELECT COUNT(*) FROM ctrace_net_rx;") == 25);
    assert(query_count(db, "SELECT COUNT(*) FROM ctrace_api_call WHERE url = 'http://api.local/query' AND active = 1;") == 1);

    // Struct arrays and the active union variant are stored as serialized blobs, in their own columns
    char row[256];
    query_text(db, "SELECT hex(batch_a) || '|' || hex(batch_b) || '|' || hex(batch_u) || '|' || batch_tail FROM ctrace_sq_batch;", row, sizeof(row));
    if (strcmp(row, "010001020000|030001|DDCCBBAA|77") != 0) {
        fprintf(stderr, "unexpected sq_batch row: %s\n", row);
        assert(0);
    }
    sqlite3_close(db);

    // Run 2: Verify re-initialization overwrites cleanly with fresh run results
    ctrace_init("test_sqlite.db");
    TRACE_EVENT(net_rx, socket_fd, len, payload);
    ctrace_shutdown();

    rc = sqlite3_open_v2("test_sqlite.db", &db, SQLITE_OPEN_READONLY, NULL);
    assert(rc == SQLITE_OK && db != NULL);
    assert(query_count(db, "SELECT COUNT(*) FROM ctrace_net_rx;") == 1);
    sqlite3_close(db);

    printf("test_sqlite passed successfully!\n");
    return 0;
}
