#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sqlite3.h>
#include <time.h>

static sqlite3 *g_db = NULL;
static pthread_mutex_t g_sqlite_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_in_transaction = 0;
static int g_batch_count = 0;
static struct timespec g_last_commit_ts;

typedef void (*ctrace_sqlite_init_fn)(sqlite3 *db);
typedef void (*ctrace_sqlite_cleanup_fn)(void);

static ctrace_sqlite_init_fn g_sqlite_init_fn = NULL;
static ctrace_sqlite_cleanup_fn g_sqlite_cleanup_fn = NULL;

void ctrace_sqlite_register_init(ctrace_sqlite_init_fn fn) {
    pthread_mutex_lock(&g_sqlite_mutex);
    g_sqlite_init_fn = fn;
    if (g_db && fn) {
        fn(g_db);
    }
    pthread_mutex_unlock(&g_sqlite_mutex);
}

void ctrace_sqlite_register_cleanup(ctrace_sqlite_cleanup_fn fn) {
    pthread_mutex_lock(&g_sqlite_mutex);
    g_sqlite_cleanup_fn = fn;
    pthread_mutex_unlock(&g_sqlite_mutex);
}

__attribute__((weak)) void __ctrace_sqlite_init_events(sqlite3 *db) {
    (void)db;
}

__attribute__((weak)) void __ctrace_sqlite_finalize_events(void) {
}

static void close_sqlite_db_locked(void) {
    if (!g_db) return;

    if (g_in_transaction) {
        sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
        g_in_transaction = 0;
    }
    if (g_sqlite_cleanup_fn) {
        g_sqlite_cleanup_fn();
    } else if (__ctrace_sqlite_finalize_events) {
        __ctrace_sqlite_finalize_events();
    }
    sqlite3_close_v2(g_db);
    g_db = NULL;
}

void ctrace_sqlite_lock(void) {
    pthread_mutex_lock(&g_sqlite_mutex);
}

void ctrace_sqlite_unlock(void) {
    pthread_mutex_unlock(&g_sqlite_mutex);
}

void ctrace_sqlite_step_done(void) {
    g_batch_count++;
    if (g_batch_count >= 1000) {
        sqlite3_exec(g_db, "COMMIT; BEGIN TRANSACTION;", NULL, NULL, NULL);
        g_batch_count = 0;
        clock_gettime(CLOCK_MONOTONIC, &g_last_commit_ts);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t diff_ms = (now.tv_sec - g_last_commit_ts.tv_sec) * 1000 +
                      (now.tv_nsec - g_last_commit_ts.tv_nsec) / 1000000;
    if (diff_ms >= 10) {
        sqlite3_exec(g_db, "COMMIT; BEGIN TRANSACTION;", NULL, NULL, NULL);
        g_batch_count = 0;
        g_last_commit_ts = now;
    }
}

void ctrace_init(const char *target_uri) {
    ctrace_calibrate_tsc();
    pthread_mutex_lock(&g_sqlite_mutex);

    close_sqlite_db_locked();

    const char *db_path = target_uri;
    if (!db_path || strcmp(db_path, "") == 0) {
        db_path = "trace.db";
    }

    if (strcmp(db_path, ":memory:") != 0) {
        unlink(db_path);
    }

    int rc = sqlite3_open_v2(db_path, &g_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
                             NULL);
    if (rc != SQLITE_OK || !g_db) {
        fprintf(stderr, "[ctrace] Failed to open SQLite database '%s': %s\n",
                db_path, g_db ? sqlite3_errmsg(g_db) : "out of memory");
        if (g_db) {
            sqlite3_close_v2(g_db);
            g_db = NULL;
        }
        pthread_mutex_unlock(&g_sqlite_mutex);
        return;
    }

    // Write-only trace capture PRAGMAs for high-throughput disk persistence
    sqlite3_exec(g_db, "PRAGMA synchronous=OFF;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA journal_mode=MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);

    // Initialize tables and prepared statements
    if (g_sqlite_init_fn) {
        g_sqlite_init_fn(g_db);
    } else if (__ctrace_sqlite_init_events) {
        __ctrace_sqlite_init_events(g_db);
    }

    sqlite3_exec(g_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    g_in_transaction = 1;
    g_batch_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_last_commit_ts);

    pthread_mutex_unlock(&g_sqlite_mutex);
}

void ctrace_flush(void) {
    pthread_mutex_lock(&g_sqlite_mutex);
    if (g_db && g_in_transaction) {
        sqlite3_exec(g_db, "COMMIT; BEGIN TRANSACTION;", NULL, NULL, NULL);
        g_batch_count = 0;
        clock_gettime(CLOCK_MONOTONIC, &g_last_commit_ts);
    }
    pthread_mutex_unlock(&g_sqlite_mutex);
}

void ctrace_shutdown(void) {
    pthread_mutex_lock(&g_sqlite_mutex);
    close_sqlite_db_locked();
    pthread_mutex_unlock(&g_sqlite_mutex);
}
