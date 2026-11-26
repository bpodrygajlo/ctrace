#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sqlite3.h>

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

    ctrace_flush();
    ctrace_shutdown();

    // Offline investigation: open the saved SQLite file directly with sqlite3
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("test_sqlite.db", &db, SQLITE_OPEN_READONLY, NULL);
    assert(rc == SQLITE_OK && db != NULL);
    assert(query_count(db, "SELECT COUNT(*) FROM ctrace_net_rx;") == 25);
    assert(query_count(db, "SELECT COUNT(*) FROM ctrace_api_call WHERE url = 'http://api.local/query' AND active = 1;") == 1);
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
