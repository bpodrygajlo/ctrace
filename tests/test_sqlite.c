#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static int g_query_row_count = 0;
static int count_cb(void *arg, int argc, char **argv, char **colNames) {
    (void)arg; (void)colNames;
    if (argc > 0 && argv[0]) {
        g_query_row_count = atoi(argv[0]);
    }
    return 0;
}

int main(void) {
    ctrace_init(":memory:");

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

    // Query in-memory DB
    int rc = ctrace_sql_exec("SELECT COUNT(*) FROM ctrace_net_rx;", count_cb, NULL);
    assert(rc == 0);
    assert(g_query_row_count == 25);

    rc = ctrace_sql_exec("SELECT COUNT(*) FROM ctrace_api_call;", count_cb, NULL);
    assert(rc == 0);
    assert(g_query_row_count == 1);

    // Test snapshot
    rc = ctrace_sql_snapshot("test_sqlite_snapshot.db");
    assert(rc == 0);

    ctrace_shutdown();

    // Verify snapshot file exists and is non-empty
    FILE *f = fopen("test_sqlite_snapshot.db", "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    assert(size > 0);

    // Test re-initialization after shutdown (verifying statement finalization & reset)
    ctrace_init(":memory:");
    TRACE_EVENT(net_rx, socket_fd, len, payload);
    ctrace_flush();
    g_query_row_count = 0;
    rc = ctrace_sql_exec("SELECT COUNT(*) FROM ctrace_net_rx;", count_cb, NULL);
    assert(rc == 0);
    assert(g_query_row_count == 1);
    ctrace_shutdown();

    printf("test_sqlite passed successfully!\n");
    return 0;
}
