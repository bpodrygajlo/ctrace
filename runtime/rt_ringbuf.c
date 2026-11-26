#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>

#define CTRACE_RINGBUF_SIZE (256 * 1024)
#define CTRACE_WRAP_MARKER  0xFFFF

typedef struct ctrace_thread_rb {
    uint8_t buf[CTRACE_RINGBUF_SIZE];
    _Atomic size_t write_pos;
    _Atomic size_t read_pos;
    size_t reserve_pos;
    struct ctrace_thread_rb *next;
} ctrace_thread_rb_t;

static int g_out_fd = -1;
static int g_owns_fd = 0;
static _Atomic int g_writer_running = 0;
static pthread_t g_writer_tid;
static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static ctrace_thread_rb_t *g_rb_list = NULL;
static _Thread_local ctrace_thread_rb_t *tl_rb = NULL;

static ctrace_thread_rb_t *get_or_create_thread_rb(void) {
    if (__builtin_expect(tl_rb != NULL, 1)) {
        return tl_rb;
    }

    ctrace_thread_rb_t *rb = (ctrace_thread_rb_t *)calloc(1, sizeof(ctrace_thread_rb_t));
    if (!rb) return NULL;

    atomic_init(&rb->write_pos, 0);
    atomic_init(&rb->read_pos, 0);
    rb->reserve_pos = 0;

    pthread_mutex_lock(&g_list_mutex);
    rb->next = g_rb_list;
    g_rb_list = rb;
    pthread_mutex_unlock(&g_list_mutex);

    tl_rb = rb;
    return rb;
}

void *ctrace_bin_reserve(size_t len) {
    ctrace_thread_rb_t *rb = get_or_create_thread_rb();
    if (!rb || len == 0 || len > CTRACE_RINGBUF_SIZE / 2) {
        return NULL;
    }

    size_t cur = rb->reserve_pos;
    size_t rd = atomic_load_explicit(&rb->read_pos, memory_order_acquire);

    // If fits linearly before end of buffer
    if (cur + len <= CTRACE_RINGBUF_SIZE) {
        // Check if write would catch up to read
        if (rd > cur && cur + len >= rd) {
            // Buffer full, brief spin
            usleep(100);
            rd = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
            if (rd > cur && cur + len >= rd) return NULL;
        }
        return &rb->buf[cur];
    }

    // Need to wrap to start
    // Check if space at beginning
    if (rd <= len && rd != 0) {
        usleep(100);
        rd = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        if (rd <= len && rd != 0) return NULL;
    }

    // Write wrap marker (2 bytes 0xFFFF)
    if (cur + 2 <= CTRACE_RINGBUF_SIZE) {
        uint16_t wrap = CTRACE_WRAP_MARKER;
        memcpy(&rb->buf[cur], &wrap, 2);
    }
    atomic_store_explicit(&rb->write_pos, CTRACE_RINGBUF_SIZE, memory_order_release);

    rb->reserve_pos = 0;
    return &rb->buf[0];
}

void ctrace_bin_commit(void *ptr, size_t len) {
    ctrace_thread_rb_t *rb = tl_rb;
    if (!rb || !ptr) return;

    size_t new_pos = rb->reserve_pos + len;
    if (new_pos >= CTRACE_RINGBUF_SIZE) {
        new_pos = 0;
    }
    rb->reserve_pos = new_pos;
    atomic_store_explicit(&rb->write_pos, new_pos, memory_order_release);
}

static void drain_single_rb(ctrace_thread_rb_t *rb, int fd) {
    if (fd < 0) return;

    size_t wr = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    size_t rd = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);

    if (wr == rd) return;

    if (wr == CTRACE_RINGBUF_SIZE) {
        // Read until wrap marker or end
        if (rd < CTRACE_RINGBUF_SIZE) {
            size_t available = CTRACE_RINGBUF_SIZE - rd;
            if (available >= 2) {
                uint16_t tag;
                memcpy(&tag, &rb->buf[rd], 2);
                if (tag == CTRACE_WRAP_MARKER) {
                    // Wrap marker reached
                    atomic_store_explicit(&rb->read_pos, 0, memory_order_release);
                    return;
                }
            }
            ssize_t written = write(fd, &rb->buf[rd], available);
            (void)written;
        }
        atomic_store_explicit(&rb->read_pos, 0, memory_order_release);
        return;
    }

    if (wr > rd) {
        size_t count = wr - rd;
        ssize_t written = write(fd, &rb->buf[rd], count);
        if (written > 0) {
            atomic_store_explicit(&rb->read_pos, rd + (size_t)written, memory_order_release);
        }
    } else {
        // wr < rd: buffer wrapped
        size_t count1 = CTRACE_RINGBUF_SIZE - rd;
        if (count1 >= 2) {
            uint16_t tag;
            memcpy(&tag, &rb->buf[rd], 2);
            if (tag == CTRACE_WRAP_MARKER) {
                count1 = 0; // Skip writing marker
            }
        }
        if (count1 > 0) {
            ssize_t written = write(fd, &rb->buf[rd], count1);
            (void)written;
        }
        atomic_store_explicit(&rb->read_pos, 0, memory_order_release);

        if (wr > 0) {
            ssize_t written = write(fd, &rb->buf[0], wr);
            if (written > 0) {
                atomic_store_explicit(&rb->read_pos, (size_t)written, memory_order_release);
            }
        }
    }
}

static void drain_all_buffers(int fd) {
    pthread_mutex_lock(&g_list_mutex);
    ctrace_thread_rb_t *curr = g_rb_list;
    while (curr) {
        drain_single_rb(curr, fd);
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_list_mutex);
}

static int g_configured_writer_cpu = -1;
static int g_configured_writer_nice = 0;
static int g_has_configured_writer_nice = 0;

void ctrace_set_writer_affinity(int cpu_core) {
    g_configured_writer_cpu = cpu_core;
    if (atomic_load(&g_writer_running)) {
#if defined(__linux__)
        if (cpu_core >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core, &cpuset);
            pthread_setaffinity_np(g_writer_tid, sizeof(cpu_set_t), &cpuset);
        }
#endif
    }
}

void ctrace_set_writer_priority(int nice_val) {
    g_configured_writer_nice = nice_val;
    g_has_configured_writer_nice = 1;
}

static void *writer_thread_func(void *arg) {
    (void)arg;

#if defined(__linux__)
    // 1. Thread naming for system tools (top, htop, bcc, perf)
    pthread_setname_np(pthread_self(), "ctrace-writer");

    // 2. Real-Time Guard: Never allow writer thread to run with real-time scheduling
    int cur_policy = 0;
    struct sched_param cur_param;
    if (pthread_getschedparam(pthread_self(), &cur_policy, &cur_param) == 0) {
        if (cur_policy == SCHED_FIFO || cur_policy == SCHED_RR) {
            struct sched_param norm_param = { .sched_priority = 0 };
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &norm_param);
        }
    }

    // 3. CPU Affinity configuration
    int target_cpu = g_configured_writer_cpu;
    if (target_cpu < 0) {
        const char *env_cpu = getenv("CTRACE_WRITER_CPU");
        if (env_cpu && *env_cpu) {
            target_cpu = atoi(env_cpu);
        }
    }
    if (target_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(target_cpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }

    // 4. Nice priority configuration
    int nice_val = 0;
    int apply_nice = g_has_configured_writer_nice;
    if (apply_nice) {
        nice_val = g_configured_writer_nice;
    } else {
        const char *env_nice = getenv("CTRACE_WRITER_PRIORITY");
        if (!env_nice) env_nice = getenv("CTRACE_WRITER_NICE");
        if (env_nice && *env_nice) {
            nice_val = atoi(env_nice);
            apply_nice = 1;
        }
    }
    if (apply_nice) {
        setpriority(PRIO_PROCESS, 0, nice_val);
    }
#endif

    while (atomic_load_explicit(&g_writer_running, memory_order_relaxed)) {
        drain_all_buffers(g_out_fd);
        usleep(2000); // 2ms drain interval
    }
    drain_all_buffers(g_out_fd);
    return NULL;
}

void ctrace_init(const char *target_uri) {
    if (g_out_fd >= 0) {
        ctrace_shutdown();
    }

    const char *path = target_uri;
    if (!path || strcmp(path, "") == 0) {
        path = "trace.bin";
    }

    if (strcmp(path, "stdout") == 0) {
        g_out_fd = STDOUT_FILENO;
        g_owns_fd = 0;
    } else if (strcmp(path, "stderr") == 0) {
        g_out_fd = STDERR_FILENO;
        g_owns_fd = 0;
    } else {
        g_out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_out_fd < 0) {
            g_out_fd = STDOUT_FILENO;
            g_owns_fd = 0;
        } else {
            g_owns_fd = 1;
        }
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // If caller thread runs with real-time priority, explicitly decouple writer thread
    int caller_policy = 0;
    struct sched_param caller_param;
    if (pthread_getschedparam(pthread_self(), &caller_policy, &caller_param) == 0) {
        if (caller_policy == SCHED_FIFO || caller_policy == SCHED_RR) {
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
            pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
            struct sched_param norm_param = { .sched_priority = 0 };
            pthread_attr_setschedparam(&attr, &norm_param);
        }
    }

    atomic_store(&g_writer_running, 1);
    int rc = pthread_create(&g_writer_tid, &attr, writer_thread_func, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_store(&g_writer_running, 0);
        fprintf(stderr, "ctrace warning: Failed to create background writer thread (errno %d)\n", rc);
    }
}

void ctrace_flush(void) {
    if (g_out_fd >= 0) {
        drain_all_buffers(g_out_fd);
        fsync(g_out_fd);
    }
}

void ctrace_shutdown(void) {
    if (atomic_load(&g_writer_running)) {
        atomic_store(&g_writer_running, 0);
        pthread_join(g_writer_tid, NULL);
    }
    if (g_out_fd >= 0) {
        drain_all_buffers(g_out_fd);
        if (g_owns_fd) {
            close(g_out_fd);
        }
        g_out_fd = -1;
        g_owns_fd = 0;
    }
}
