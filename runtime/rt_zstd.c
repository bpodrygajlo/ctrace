#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <zstd.h>

#define CTRACE_ZSTD_CHUNK_SIZE (64 * 1024)

typedef struct ctrace_zstd_thread {
    pthread_mutex_t lock;
    char raw_buf[CTRACE_ZSTD_CHUNK_SIZE];
    size_t raw_len;
    ZSTD_CCtx *cctx;
    char comp_buf[ZSTD_COMPRESSBOUND(CTRACE_ZSTD_CHUNK_SIZE) + 4];
    struct ctrace_zstd_thread *next;
} ctrace_zstd_thread_t;

static _Atomic int g_zstd_fd = -1;
static int g_zstd_owns_fd = 0;
static pthread_mutex_t g_zstd_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_zstd_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(ctrace_zstd_thread_t *) g_zstd_threads = NULL;
static _Thread_local ctrace_zstd_thread_t *tl_zstd = NULL;

static void zstd_write_all(int fd, const char *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t w = write(fd, buf + total, count - total);
        if (w > 0) {
            total += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static ctrace_zstd_thread_t *get_or_create_zstd_thread(void) {
    if (__builtin_expect(tl_zstd != NULL, 1)) {
        return tl_zstd;
    }

    ctrace_zstd_thread_t *t = (ctrace_zstd_thread_t *)calloc(1, sizeof(ctrace_zstd_thread_t));
    if (!t) return NULL;

    t->cctx = ZSTD_createCCtx();
    if (!t->cctx) {
        free(t);
        return NULL;
    }
    pthread_mutex_init(&t->lock, NULL);
    t->raw_len = 0;

    pthread_mutex_lock(&g_zstd_list_mutex);
    t->next = atomic_load_explicit(&g_zstd_threads, memory_order_relaxed);
    atomic_store_explicit(&g_zstd_threads, t, memory_order_release);
    pthread_mutex_unlock(&g_zstd_list_mutex);

    tl_zstd = t;
    return t;
}

// Caller must hold t->lock
static void flush_thread_chunk_locked(ctrace_zstd_thread_t *t) {
    if (!t || t->raw_len == 0) return;
    int fd = atomic_load_explicit(&g_zstd_fd, memory_order_acquire);
    if (fd >= 0 && t->cctx) {
        size_t max_comp = ZSTD_COMPRESSBOUND(CTRACE_ZSTD_CHUNK_SIZE);
        size_t c_size = ZSTD_compressCCtx(t->cctx, t->comp_buf + 4, max_comp, t->raw_buf, t->raw_len, 1);
        if (!ZSTD_isError(c_size)) {
            uint32_t c_len = (uint32_t)c_size;
            memcpy(t->comp_buf, &c_len, 4);

            pthread_mutex_lock(&g_zstd_file_mutex);
            fd = atomic_load_explicit(&g_zstd_fd, memory_order_acquire);
            if (fd >= 0) {
                zstd_write_all(fd, t->comp_buf, 4 + c_size);
            }
            pthread_mutex_unlock(&g_zstd_file_mutex);
        }
    }
    t->raw_len = 0;
}

void *ctrace_bin_reserve(size_t len) {
    if (atomic_load_explicit(&g_zstd_fd, memory_order_acquire) < 0) {
        return NULL;
    }
    ctrace_zstd_thread_t *t = get_or_create_zstd_thread();
    if (!t || len == 0 || len > CTRACE_ZSTD_CHUNK_SIZE) {
        return NULL;
    }

    pthread_mutex_lock(&t->lock);
    if (t->raw_len + len > CTRACE_ZSTD_CHUNK_SIZE) {
        flush_thread_chunk_locked(t);
    }

    return &t->raw_buf[t->raw_len];
}

void ctrace_bin_commit(void *ptr, size_t len) {
    ctrace_zstd_thread_t *t = tl_zstd;
    if (!t || !ptr) return;

    t->raw_len += len;
    pthread_mutex_unlock(&t->lock);
}

void ctrace_init(const char *target_uri) {
    ctrace_calibrate_tsc();
    if (atomic_load_explicit(&g_zstd_fd, memory_order_acquire) >= 0) {
        ctrace_shutdown();
    }

    const char *path = target_uri;
    if (!path || strcmp(path, "") == 0) {
        path = "trace.bin.zst";
    }

    pthread_mutex_lock(&g_zstd_file_mutex);
    if (strcmp(path, "stdout") == 0) {
        atomic_store_explicit(&g_zstd_fd, STDOUT_FILENO, memory_order_release);
        g_zstd_owns_fd = 0;
    } else if (strcmp(path, "stderr") == 0) {
        atomic_store_explicit(&g_zstd_fd, STDERR_FILENO, memory_order_release);
        g_zstd_owns_fd = 0;
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "ctrace error: Failed to open zstd trace file '%s'\n", path);
            atomic_store_explicit(&g_zstd_fd, -1, memory_order_release);
            g_zstd_owns_fd = 0;
        } else {
            atomic_store_explicit(&g_zstd_fd, fd, memory_order_release);
            g_zstd_owns_fd = 1;
        }
    }
    pthread_mutex_unlock(&g_zstd_file_mutex);
}

void ctrace_flush(void) {
    ctrace_zstd_thread_t *curr = atomic_load_explicit(&g_zstd_threads, memory_order_acquire);
    while (curr) {
        pthread_mutex_lock(&curr->lock);
        flush_thread_chunk_locked(curr);
        pthread_mutex_unlock(&curr->lock);
        curr = curr->next;
    }

    pthread_mutex_lock(&g_zstd_file_mutex);
    int fd = atomic_load_explicit(&g_zstd_fd, memory_order_acquire);
    if (fd >= 0) {
        (void)fsync(fd);
    }
    pthread_mutex_unlock(&g_zstd_file_mutex);
}

void ctrace_shutdown(void) {
    ctrace_flush();

    pthread_mutex_lock(&g_zstd_file_mutex);
    int fd = atomic_load_explicit(&g_zstd_fd, memory_order_acquire);
    if (fd >= 0) {
        if (g_zstd_owns_fd) {
            close(fd);
        }
        atomic_store_explicit(&g_zstd_fd, -1, memory_order_release);
        g_zstd_owns_fd = 0;
    }
    pthread_mutex_unlock(&g_zstd_file_mutex);
}
