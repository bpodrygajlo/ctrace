#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>

#if defined(CTRACE_MULTI_BACKEND)
void ctrace_shutdown_ringbuf(void);
#endif

#define CTRACE_RINGBUF_SIZE   (256 * 1024)
#define CTRACE_MAX_EVENT_SIZE ((CTRACE_RINGBUF_SIZE / 2) - 64)

typedef struct ctrace_thread_rb {
    alignas(64) uint8_t buf[CTRACE_RINGBUF_SIZE];
    alignas(64) _Atomic size_t write_pos;
    _Atomic size_t wrap_at;
    size_t reserve_pos;
    alignas(64) _Atomic size_t read_pos;
    alignas(64) struct ctrace_thread_rb *next;
} ctrace_thread_rb_t;

static _Atomic int g_out_fd = -1;
static int g_owns_fd = 0;
static _Atomic int g_writer_running = 0;
static int g_writer_tid_valid = 0;
static pthread_t g_writer_tid;

static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_drain_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

static _Atomic(ctrace_thread_rb_t *) g_rb_list = NULL;
static _Thread_local ctrace_thread_rb_t *tl_rb = NULL;

static _Atomic int g_configured_writer_cpu = -1;
static _Atomic int g_configured_writer_nice = 0;
static _Atomic int g_has_configured_writer_nice = 0;

static void sleep_ns(long ns) {
    struct timespec req = { .tv_sec = ns / 1000000000L, .tv_nsec = ns % 1000000000L };
    nanosleep(&req, NULL);
}

static size_t write_all(int fd, const uint8_t *buf, size_t count) {
    size_t total = 0;
    int retries = 0;
    while (total < count) {
        ssize_t w = write(fd, buf + total, count - total);
        if (w > 0) {
            total += (size_t)w;
            retries = 0;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && retries < 5) {
            retries++;
            sleep_ns(50000L);
        } else {
            break;
        }
    }
    return total;
}

static ctrace_thread_rb_t *get_or_create_thread_rb(void) {
    if (__builtin_expect(tl_rb != NULL, 1)) {
        return tl_rb;
    }

    ctrace_thread_rb_t *rb = NULL;
    if (posix_memalign((void **)&rb, 64, sizeof(ctrace_thread_rb_t)) != 0 || !rb) {
        return NULL;
    }
    memset(rb, 0, sizeof(ctrace_thread_rb_t));

    atomic_init(&rb->write_pos, 0);
    atomic_init(&rb->wrap_at, CTRACE_RINGBUF_SIZE);
    atomic_init(&rb->read_pos, 0);
    rb->reserve_pos = 0;

    pthread_mutex_lock(&g_list_mutex);
    rb->next = atomic_load_explicit(&g_rb_list, memory_order_relaxed);
    atomic_store_explicit(&g_rb_list, rb, memory_order_release);
    pthread_mutex_unlock(&g_list_mutex);

    tl_rb = rb;
    return rb;
}

void *ctrace_bin_reserve(size_t len) {
    ctrace_thread_rb_t *rb = get_or_create_thread_rb();
    if (!rb || len == 0 || len > CTRACE_MAX_EVENT_SIZE) {
        return NULL;
    }

    size_t cur = rb->reserve_pos;
    size_t rd = atomic_load_explicit(&rb->read_pos, memory_order_acquire);

    // Case 1: Fits strictly before the end of the physical buffer
    if (cur + len < CTRACE_RINGBUF_SIZE) {
        if (rd > cur && cur + len >= rd) {
            sleep_ns(100000L); // 100us backpressure yield
            rd = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
            if (rd > cur && cur + len >= rd) {
                return NULL;
            }
        }
        return &rb->buf[cur];
    }

    // Case 2: Wrap to start of buffer (0 .. len).
    // Safe iff consumer has drained any prior tail (rd <= cur) and cleared [0 .. len] (rd > len).
    if (!(rd > len && rd <= cur)) {
        sleep_ns(100000L); // 100us backpressure yield
        rd = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
        if (!(rd > len && rd <= cur)) {
            return NULL;
        }
    }

    atomic_store_explicit(&rb->wrap_at, cur, memory_order_relaxed);
    rb->reserve_pos = 0;
    return &rb->buf[0];
}

void ctrace_bin_commit(void *ptr, size_t len) {
    ctrace_thread_rb_t *rb = tl_rb;
    if (!rb || !ptr) return;

    size_t new_pos = rb->reserve_pos + len;
    rb->reserve_pos = new_pos;
    atomic_store_explicit(&rb->write_pos, new_pos, memory_order_release);
}

static void drain_single_rb(ctrace_thread_rb_t *rb, int fd) {
    if (fd < 0) return;

    size_t wr = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    size_t rd = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);

    if (wr == rd) return;

    if (wr > rd) {
        size_t count = wr - rd;
        size_t written = write_all(fd, &rb->buf[rd], count);
        if (written > 0) {
            atomic_store_explicit(&rb->read_pos, rd + written, memory_order_release);
        }
    } else {
        // wr < rd: Producer wrapped at wrap_at
        size_t end = atomic_load_explicit(&rb->wrap_at, memory_order_relaxed);
        if (end > CTRACE_RINGBUF_SIZE) {
            end = CTRACE_RINGBUF_SIZE;
        }
        if (rd < end) {
            size_t count1 = end - rd;
            size_t written1 = write_all(fd, &rb->buf[rd], count1);
            if (written1 < count1) {
                if (written1 > 0) {
                    atomic_store_explicit(&rb->read_pos, rd + written1, memory_order_release);
                }
                return;
            }
        }
        atomic_store_explicit(&rb->read_pos, 0, memory_order_release);

        if (wr > 0) {
            size_t written2 = write_all(fd, &rb->buf[0], wr);
            if (written2 > 0) {
                atomic_store_explicit(&rb->read_pos, written2, memory_order_release);
            }
        }
    }
}

static void drain_all_buffers(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_drain_mutex);
    ctrace_thread_rb_t *curr = atomic_load_explicit(&g_rb_list, memory_order_acquire);
    while (curr) {
        drain_single_rb(curr, fd);
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_drain_mutex);
}

void ctrace_set_writer_affinity(int cpu_core) {
    atomic_store_explicit(&g_configured_writer_cpu, cpu_core, memory_order_release);
    pthread_mutex_lock(&g_lifecycle_mutex);
    if (g_writer_tid_valid) {
#if defined(__linux__)
        if (cpu_core >= 0 && cpu_core < CPU_SETSIZE) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core, &cpuset);
            (void)pthread_setaffinity_np(g_writer_tid, sizeof(cpu_set_t), &cpuset);
        }
#endif
    }
    pthread_mutex_unlock(&g_lifecycle_mutex);
}

void ctrace_set_writer_priority(int nice_val) {
    atomic_store_explicit(&g_configured_writer_nice, nice_val, memory_order_relaxed);
    atomic_store_explicit(&g_has_configured_writer_nice, 1, memory_order_release);
}

static void *writer_thread_func(void *arg) {
    (void)arg;

#if defined(__linux__)
    // 1. Thread naming for system tools (top, htop, bcc, perf)
    (void)pthread_setname_np(pthread_self(), "ctrace-writer");

    // 2. Real-Time Guard: Never allow writer thread to run with real-time scheduling
    int cur_policy = 0;
    struct sched_param cur_param;
    if (pthread_getschedparam(pthread_self(), &cur_policy, &cur_param) == 0) {
        if (cur_policy == SCHED_FIFO || cur_policy == SCHED_RR) {
            struct sched_param norm_param = { .sched_priority = 0 };
            (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &norm_param);
        }
    }

    // 3. CPU Affinity configuration
    int target_cpu = atomic_load_explicit(&g_configured_writer_cpu, memory_order_acquire);
    if (target_cpu < 0) {
        const char *env_cpu = getenv("CTRACE_WRITER_CPU");
        if (env_cpu && *env_cpu) {
            target_cpu = atoi(env_cpu);
        }
    }
    if (target_cpu >= 0 && target_cpu < CPU_SETSIZE) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(target_cpu, &cpuset);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }

    // 4. Nice priority configuration
    int nice_val = 0;
    int apply_nice = atomic_load_explicit(&g_has_configured_writer_nice, memory_order_acquire);
    if (apply_nice) {
        nice_val = atomic_load_explicit(&g_configured_writer_nice, memory_order_relaxed);
    } else {
        const char *env_nice = getenv("CTRACE_WRITER_PRIORITY");
        if (!env_nice) env_nice = getenv("CTRACE_WRITER_NICE");
        if (env_nice && *env_nice) {
            nice_val = atoi(env_nice);
            apply_nice = 1;
        }
    }
    if (apply_nice) {
        (void)setpriority(PRIO_PROCESS, 0, nice_val);
    }
#endif

    while (atomic_load_explicit(&g_writer_running, memory_order_acquire)) {
        drain_all_buffers(atomic_load_explicit(&g_out_fd, memory_order_acquire));
        sleep_ns(2000000L); // 2ms drain interval
    }
    drain_all_buffers(atomic_load_explicit(&g_out_fd, memory_order_acquire));
    return NULL;
}

#if defined(CTRACE_MULTI_BACKEND)
void ctrace_init_ringbuf(const char *target_uri)
#else
void ctrace_init(const char *target_uri)
#endif
{
    ctrace_calibrate_tsc();
    if (atomic_load_explicit(&g_out_fd, memory_order_acquire) >= 0) {
#if defined(CTRACE_MULTI_BACKEND)
        ctrace_shutdown_ringbuf();
#else
        ctrace_shutdown();
#endif
    }

    pthread_mutex_lock(&g_lifecycle_mutex);

    const char *path = target_uri;
    if (!path || strcmp(path, "") == 0) {
        path = getenv("CTRACE_OUTPUT_PATH");
        if (!path || strcmp(path, "") == 0) {
            path = getenv("CTRACE_FILE");
        }
    }
    if (!path || strcmp(path, "") == 0) {
        path = "trace.bin";
    }

    int fd = -1;
    int owns = 0;
    if (strcmp(path, "stdout") == 0) {
        fd = STDOUT_FILENO;
        owns = 0;
    } else if (strcmp(path, "stderr") == 0) {
        fd = STDERR_FILENO;
        owns = 0;
    } else {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "ctrace error: Failed to open binary trace file '%s'\n", path);
            pthread_mutex_unlock(&g_lifecycle_mutex);
            return;
        }
        owns = 1;
    }

    atomic_store_explicit(&g_out_fd, fd, memory_order_release);
    g_owns_fd = owns;

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    int caller_policy = 0;
    struct sched_param caller_param;
    if (pthread_getschedparam(pthread_self(), &caller_policy, &caller_param) == 0) {
        if (caller_policy == SCHED_FIFO || caller_policy == SCHED_RR) {
            (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
            (void)pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
            struct sched_param norm_param = { .sched_priority = 0 };
            (void)pthread_attr_setschedparam(&attr, &norm_param);
        }
    }

    atomic_store_explicit(&g_writer_running, 1, memory_order_release);
    int rc = pthread_create(&g_writer_tid, &attr, writer_thread_func, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_store_explicit(&g_writer_running, 0, memory_order_release);
        g_writer_tid_valid = 0;
        fprintf(stderr, "ctrace warning: Failed to create background writer thread (errno %d)\n", rc);
    } else {
        g_writer_tid_valid = 1;
    }

    pthread_mutex_unlock(&g_lifecycle_mutex);
}

#if defined(CTRACE_MULTI_BACKEND)
void ctrace_flush_ringbuf(void)
#else
void ctrace_flush(void)
#endif
{
    int fd = atomic_load_explicit(&g_out_fd, memory_order_acquire);
    if (fd >= 0) {
        drain_all_buffers(fd);
        (void)fsync(fd);
    }
}

#if defined(CTRACE_MULTI_BACKEND)
void ctrace_shutdown_ringbuf(void)
#else
void ctrace_shutdown(void)
#endif
{
    pthread_mutex_lock(&g_lifecycle_mutex);
    if (g_writer_tid_valid) {
        atomic_store_explicit(&g_writer_running, 0, memory_order_release);
        pthread_t tid = g_writer_tid;
        g_writer_tid_valid = 0;
        pthread_mutex_unlock(&g_lifecycle_mutex);
        pthread_join(tid, NULL);
        pthread_mutex_lock(&g_lifecycle_mutex);
    }

    int fd = atomic_load_explicit(&g_out_fd, memory_order_acquire);
    if (fd >= 0) {
        drain_all_buffers(fd);
        (void)fsync(fd);
        if (g_owns_fd) {
            close(fd);
        }
        atomic_store_explicit(&g_out_fd, -1, memory_order_release);
        g_owns_fd = 0;
    }
    pthread_mutex_unlock(&g_lifecycle_mutex);
}
