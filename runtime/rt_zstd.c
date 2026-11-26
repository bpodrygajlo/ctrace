#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <zstd.h>

#define CTRACE_ZSTD_CHUNK_SIZE (64 * 1024)

typedef struct ctrace_zstd_thread {
    char raw_buf[CTRACE_ZSTD_CHUNK_SIZE];
    size_t raw_len;
    ZSTD_CCtx *cctx;
    char comp_buf[ZSTD_COMPRESSBOUND(CTRACE_ZSTD_CHUNK_SIZE) + 4];
    struct ctrace_zstd_thread *next;
} ctrace_zstd_thread_t;

static int g_zstd_fd = -1;
static int g_zstd_owns_fd = 0;
static pthread_mutex_t g_zstd_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_zstd_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static ctrace_zstd_thread_t *g_zstd_threads = NULL;
static _Thread_local ctrace_zstd_thread_t *tl_zstd = NULL;

static ctrace_zstd_thread_t *get_or_create_zstd_thread(void) {
    if (__builtin_expect(tl_zstd != NULL, 1)) {
        return tl_zstd;
    }

    ctrace_zstd_thread_t *t = (ctrace_zstd_thread_t *)calloc(1, sizeof(ctrace_zstd_thread_t));
    if (!t) return NULL;

    t->cctx = ZSTD_createCCtx();
    t->raw_len = 0;

    pthread_mutex_lock(&g_zstd_list_mutex);
    t->next = g_zstd_threads;
    g_zstd_threads = t;
    pthread_mutex_unlock(&g_zstd_list_mutex);

    tl_zstd = t;
    return t;
}

static void flush_thread_chunk(ctrace_zstd_thread_t *t) {
    if (!t || t->raw_len == 0 || g_zstd_fd < 0) return;

    size_t max_comp = ZSTD_COMPRESSBOUND(CTRACE_ZSTD_CHUNK_SIZE);
    size_t c_size = ZSTD_compressCCtx(t->cctx, t->comp_buf + 4, max_comp, t->raw_buf, t->raw_len, 1);
    if (!ZSTD_isError(c_size)) {
        uint32_t c_len = (uint32_t)c_size;
        memcpy(t->comp_buf, &c_len, 4);

        pthread_mutex_lock(&g_zstd_file_mutex);
        ssize_t written = write(g_zstd_fd, t->comp_buf, 4 + c_size);
        (void)written;
        pthread_mutex_unlock(&g_zstd_file_mutex);
    }
    t->raw_len = 0;
}

void *ctrace_bin_reserve(size_t len) {
    ctrace_zstd_thread_t *t = get_or_create_zstd_thread();
    if (!t || len == 0 || len > CTRACE_ZSTD_CHUNK_SIZE) {
        return NULL;
    }

    if (t->raw_len + len > CTRACE_ZSTD_CHUNK_SIZE) {
        flush_thread_chunk(t);
    }

    return &t->raw_buf[t->raw_len];
}

void ctrace_bin_commit(void *ptr, size_t len) {
    ctrace_zstd_thread_t *t = tl_zstd;
    if (!t || !ptr) return;

    t->raw_len += len;
}

void ctrace_init(const char *target_uri) {
    if (g_zstd_fd >= 0) {
        ctrace_shutdown();
    }

    const char *path = target_uri;
    if (!path || strcmp(path, "") == 0) {
        path = "trace.bin.zst";
    }

    if (strcmp(path, "stdout") == 0) {
        g_zstd_fd = STDOUT_FILENO;
        g_zstd_owns_fd = 0;
    } else if (strcmp(path, "stderr") == 0) {
        g_zstd_fd = STDERR_FILENO;
        g_zstd_owns_fd = 0;
    } else {
        g_zstd_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_zstd_fd < 0) {
            g_zstd_fd = STDOUT_FILENO;
            g_zstd_owns_fd = 0;
        } else {
            g_zstd_owns_fd = 1;
        }
    }
}

void ctrace_flush(void) {
    pthread_mutex_lock(&g_zstd_list_mutex);
    ctrace_zstd_thread_t *curr = g_zstd_threads;
    while (curr) {
        flush_thread_chunk(curr);
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_zstd_list_mutex);

    pthread_mutex_lock(&g_zstd_file_mutex);
    if (g_zstd_fd >= 0) {
        fsync(g_zstd_fd);
    }
    pthread_mutex_unlock(&g_zstd_file_mutex);
}

void ctrace_shutdown(void) {
    ctrace_flush();

    pthread_mutex_lock(&g_zstd_file_mutex);
    if (g_zstd_fd >= 0) {
        if (g_zstd_owns_fd) {
            close(g_zstd_fd);
        }
        g_zstd_fd = -1;
        g_zstd_owns_fd = 0;
    }
    pthread_mutex_unlock(&g_zstd_file_mutex);
}
