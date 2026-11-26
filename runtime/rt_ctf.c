#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>

#define CTF_MAGIC 0xC1FC1FC1
#define CTF_PACKET_SIZE 65536 // 64 KB

static const uint8_t CTF_UUID[16] = {
    0x2a, 0x85, 0x62, 0xa0, 0xbb, 0x2a, 0x46, 0x6c,
    0x94, 0x7e, 0x8c, 0x38, 0xbb, 0x4e, 0x42, 0x76
};

#pragma pack(push, 1)
typedef struct {
    // Packet Header
    uint32_t magic;
    uint8_t  uuid[16];
    uint32_t stream_id;

    // Packet Context
    uint64_t timestamp_begin;
    uint64_t timestamp_end;
    uint64_t content_size; // in bits
    uint64_t packet_size;  // in bits
    uint64_t events_discarded;
    uint32_t cpu_id;
} ctf_packet_header_t;
#pragma pack(pop)

typedef struct ctrace_ctf_thread {
    pthread_mutex_t lock;
    char packet[CTF_PACKET_SIZE];
    size_t offset;
    uint64_t ts_begin;
    uint64_t ts_end;
    size_t event_count;
    uint64_t discarded_count;
    struct ctrace_ctf_thread *next;
} ctrace_ctf_thread_t;

static _Atomic int g_ctf_fd = -1;
static char g_ctf_dir[512] = "trace_ctf";
static pthread_mutex_t g_ctf_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ctf_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(ctrace_ctf_thread_t *) g_ctf_threads = NULL;
static _Thread_local ctrace_ctf_thread_t *tl_ctf = NULL;
static const char *g_ctf_embedded_metadata = NULL;

void ctrace_ctf_set_metadata(const char *metadata) {
    g_ctf_embedded_metadata = metadata;
}

static void ctf_write_all(int fd, const char *buf, size_t count) {
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

static ctrace_ctf_thread_t *get_or_create_ctf_thread(void) {
    if (__builtin_expect(tl_ctf != NULL, 1)) {
        return tl_ctf;
    }

    ctrace_ctf_thread_t *t = (ctrace_ctf_thread_t *)calloc(1, sizeof(ctrace_ctf_thread_t));
    if (!t) return NULL;

    pthread_mutex_init(&t->lock, NULL);
    t->offset = sizeof(ctf_packet_header_t);
    t->ts_begin = 0;
    t->ts_end = 0;
    t->event_count = 0;
    t->discarded_count = 0;

    pthread_mutex_lock(&g_ctf_list_mutex);
    t->next = atomic_load_explicit(&g_ctf_threads, memory_order_relaxed);
    atomic_store_explicit(&g_ctf_threads, t, memory_order_release);
    pthread_mutex_unlock(&g_ctf_list_mutex);

    tl_ctf = t;
    return t;
}

// Caller must hold t->lock
static void flush_ctf_packet_locked(ctrace_ctf_thread_t *t) {
    int fd = atomic_load_explicit(&g_ctf_fd, memory_order_acquire);
    if (!t || fd < 0) return;
    if (t->offset <= sizeof(ctf_packet_header_t) || t->event_count == 0) return;

    ctf_packet_header_t *hdr = (ctf_packet_header_t *)t->packet;
    hdr->magic = CTF_MAGIC;
    memcpy(hdr->uuid, CTF_UUID, 16);
    hdr->stream_id = 0;
    hdr->timestamp_begin = t->ts_begin;
    hdr->timestamp_end = (t->ts_end >= t->ts_begin) ? t->ts_end : t->ts_begin;
    hdr->content_size = (uint64_t)t->offset * 8;
    hdr->packet_size = (uint64_t)CTF_PACKET_SIZE * 8;
    hdr->events_discarded = t->discarded_count;
    hdr->cpu_id = 0;

    // Zero-fill padding
    memset(t->packet + t->offset, 0, CTF_PACKET_SIZE - t->offset);

    pthread_mutex_lock(&g_ctf_file_mutex);
    fd = atomic_load_explicit(&g_ctf_fd, memory_order_acquire);
    if (fd >= 0) {
        ctf_write_all(fd, t->packet, CTF_PACKET_SIZE);
    }
    pthread_mutex_unlock(&g_ctf_file_mutex);

    t->offset = sizeof(ctf_packet_header_t);
    t->ts_begin = 0;
    t->ts_end = 0;
    t->event_count = 0;
}

void *ctrace_ctf_reserve(size_t len) {
    ctrace_ctf_thread_t *t = get_or_create_ctf_thread();
    if (!t || len == 0) {
        return NULL;
    }

    pthread_mutex_lock(&t->lock);
    if (len > CTF_PACKET_SIZE - sizeof(ctf_packet_header_t)) {
        t->discarded_count++;
        pthread_mutex_unlock(&t->lock);
        return NULL;
    }

    if (t->offset + len > CTF_PACKET_SIZE) {
        flush_ctf_packet_locked(t);
    }

    return &t->packet[t->offset];
}

void ctrace_ctf_commit(void *ptr, size_t len) {
    ctrace_ctf_thread_t *t = tl_ctf;
    if (!t || !ptr) return;

    if (len >= 10) {
        uint64_t ev_ts = 0;
        memcpy(&ev_ts, (const char *)ptr + 2, sizeof(uint64_t));
        if (t->event_count == 0) {
            t->ts_begin = ev_ts;
        }
        t->ts_end = ev_ts;
    }

    t->offset += len;
    t->event_count++;
    pthread_mutex_unlock(&t->lock);
}

void ctrace_init(const char *target_uri) {
    if (atomic_load_explicit(&g_ctf_fd, memory_order_acquire) >= 0) {
        ctrace_shutdown();
    }

    const char *dir_path = target_uri;
    if (!dir_path || strcmp(dir_path, "") == 0) {
        dir_path = "trace_ctf";
    }
    snprintf(g_ctf_dir, sizeof(g_ctf_dir), "%s", dir_path);

    if (mkdir(g_ctf_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "ctrace error: Failed to create CTF directory '%s'\n", g_ctf_dir);
    }

    // Write metadata file if embedded metadata is registered
    char meta_path[560];
    snprintf(meta_path, sizeof(meta_path), "%s/metadata", g_ctf_dir);
    if (g_ctf_embedded_metadata) {
        FILE *mf = fopen(meta_path, "w");
        if (mf) {
            fputs(g_ctf_embedded_metadata, mf);
            fclose(mf);
        }
    }

    // Open stream_0 data file
    char stream_path[560];
    snprintf(stream_path, sizeof(stream_path), "%s/stream_0", g_ctf_dir);
    int fd = open(stream_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    atomic_store_explicit(&g_ctf_fd, fd, memory_order_release);
}

void ctrace_flush(void) {
    ctrace_ctf_thread_t *curr = atomic_load_explicit(&g_ctf_threads, memory_order_acquire);
    while (curr) {
        pthread_mutex_lock(&curr->lock);
        flush_ctf_packet_locked(curr);
        pthread_mutex_unlock(&curr->lock);
        curr = curr->next;
    }

    pthread_mutex_lock(&g_ctf_file_mutex);
    int fd = atomic_load_explicit(&g_ctf_fd, memory_order_acquire);
    if (fd >= 0) {
        (void)fsync(fd);
    }
    pthread_mutex_unlock(&g_ctf_file_mutex);
}

void ctrace_shutdown(void) {
    ctrace_flush();

    pthread_mutex_lock(&g_ctf_file_mutex);
    int fd = atomic_load_explicit(&g_ctf_fd, memory_order_acquire);
    if (fd >= 0) {
        close(fd);
        atomic_store_explicit(&g_ctf_fd, -1, memory_order_release);
    }
    pthread_mutex_unlock(&g_ctf_file_mutex);
}
