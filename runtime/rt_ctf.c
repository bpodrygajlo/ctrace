#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>

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
    char packet[CTF_PACKET_SIZE];
    size_t offset;
    uint64_t ts_begin;
    uint64_t ts_end;
    size_t event_count;
    struct ctrace_ctf_thread *next;
} ctrace_ctf_thread_t;

static int g_ctf_fd = -1;
static char g_ctf_dir[512] = "trace_ctf";
static pthread_mutex_t g_ctf_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ctf_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static ctrace_ctf_thread_t *g_ctf_threads = NULL;
static _Thread_local ctrace_ctf_thread_t *tl_ctf = NULL;
static const char *g_ctf_embedded_metadata = NULL;

void ctrace_ctf_set_metadata(const char *metadata) {
    g_ctf_embedded_metadata = metadata;
}

static ctrace_ctf_thread_t *get_or_create_ctf_thread(void) {
    if (__builtin_expect(tl_ctf != NULL, 1)) {
        return tl_ctf;
    }

    ctrace_ctf_thread_t *t = (ctrace_ctf_thread_t *)calloc(1, sizeof(ctrace_ctf_thread_t));
    if (!t) return NULL;

    t->offset = sizeof(ctf_packet_header_t);
    t->ts_begin = 0;
    t->ts_end = 0;
    t->event_count = 0;

    pthread_mutex_lock(&g_ctf_list_mutex);
    t->next = g_ctf_threads;
    g_ctf_threads = t;
    pthread_mutex_unlock(&g_ctf_list_mutex);

    tl_ctf = t;
    return t;
}

static void flush_ctf_packet(ctrace_ctf_thread_t *t) {
    if (!t || g_ctf_fd < 0) return;
    if (t->offset <= sizeof(ctf_packet_header_t) || t->event_count == 0) return;

    ctf_packet_header_t *hdr = (ctf_packet_header_t *)t->packet;
    hdr->magic = CTF_MAGIC;
    memcpy(hdr->uuid, CTF_UUID, 16);
    hdr->stream_id = 0;
    hdr->timestamp_begin = t->ts_begin;
    hdr->timestamp_end = (t->ts_end >= t->ts_begin) ? t->ts_end : t->ts_begin;
    hdr->content_size = (uint64_t)t->offset * 8;
    hdr->packet_size = (uint64_t)CTF_PACKET_SIZE * 8;
    hdr->events_discarded = 0;
    hdr->cpu_id = 0;

    // Zero-fill padding
    memset(t->packet + t->offset, 0, CTF_PACKET_SIZE - t->offset);

    pthread_mutex_lock(&g_ctf_file_mutex);
    ssize_t written = write(g_ctf_fd, t->packet, CTF_PACKET_SIZE);
    (void)written;
    pthread_mutex_unlock(&g_ctf_file_mutex);

    t->offset = sizeof(ctf_packet_header_t);
    t->ts_begin = 0;
    t->ts_end = 0;
    t->event_count = 0;
}

void *ctrace_ctf_reserve(size_t len) {
    ctrace_ctf_thread_t *t = get_or_create_ctf_thread();
    if (!t || len == 0 || len > CTF_PACKET_SIZE - sizeof(ctf_packet_header_t)) {
        return NULL;
    }

    if (t->offset + len > CTF_PACKET_SIZE) {
        flush_ctf_packet(t);
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
}

void ctrace_init(const char *target_uri) {
    if (g_ctf_fd >= 0) {
        ctrace_shutdown();
    }

    const char *dir_path = target_uri;
    if (!dir_path || strcmp(dir_path, "") == 0) {
        dir_path = "trace_ctf";
    }
    strncpy(g_ctf_dir, dir_path, sizeof(g_ctf_dir) - 1);

    mkdir(g_ctf_dir, 0755);

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
    g_ctf_fd = open(stream_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

void ctrace_flush(void) {
    pthread_mutex_lock(&g_ctf_list_mutex);
    ctrace_ctf_thread_t *curr = g_ctf_threads;
    while (curr) {
        flush_ctf_packet(curr);
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_ctf_list_mutex);

    pthread_mutex_lock(&g_ctf_file_mutex);
    if (g_ctf_fd >= 0) {
        fsync(g_ctf_fd);
    }
    pthread_mutex_unlock(&g_ctf_file_mutex);
}

void ctrace_shutdown(void) {
    ctrace_flush();

    pthread_mutex_lock(&g_ctf_file_mutex);
    if (g_ctf_fd >= 0) {
        close(g_ctf_fd);
        g_ctf_fd = -1;
    }
    pthread_mutex_unlock(&g_ctf_file_mutex);
}
