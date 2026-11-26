#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

static FILE *g_ctrace_out = NULL;
static int g_ctrace_owns_file = 0;
static pthread_mutex_t g_stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t g_tsc_freq = 2000000000ULL;
static uint64_t g_tsc_base_cycles = 0;
static uint64_t g_tsc_base_ns = 0;
static uint64_t g_tsc_mult = 0;
static _Atomic int g_tsc_calibrated = 0;
static pthread_once_t g_tsc_once = PTHREAD_ONCE_INIT;

static void ctrace_do_calibrate_tsc(void) {
#if (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)) && defined(__SIZEOF_INT128__)
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    uint64_t c0 = ctrace_now_cycles();

    // 1ms calibration window via POSIX nanosleep
    struct timespec req = { .tv_sec = 0, .tv_nsec = 1000000L };
    nanosleep(&req, NULL);

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    uint64_t c1 = ctrace_now_cycles();

    uint64_t delta_ns = (uint64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000ULL +
                        (uint64_t)(ts1.tv_nsec - ts0.tv_nsec);
    uint64_t delta_c = (c1 > c0) ? (c1 - c0) : 0;
    if (delta_ns == 0) delta_ns = 1;

    // Use 128-bit arithmetic so OS preemption during calibration never overflows
    uint64_t freq = (uint64_t)(((__uint128_t)delta_c * 1000000000ULL) / delta_ns);
    if (freq == 0) freq = 2000000000ULL; // fallback 2GHz

    g_tsc_freq = freq;
    g_tsc_base_cycles = c0;
    g_tsc_base_ns = (uint64_t)ts0.tv_sec * 1000000000ULL + (uint64_t)ts0.tv_nsec;

    // Fixed point factor: mult = (1e9 << 32) / freq
    g_tsc_mult = (uint64_t)(((__uint128_t)1000000000ULL << 32) / g_tsc_freq);
#else
    g_tsc_freq = 1000000000ULL;
    g_tsc_mult = 1ULL << 32;
#endif
    atomic_store_explicit(&g_tsc_calibrated, 1, memory_order_release);
}

void ctrace_calibrate_tsc(void) {
    if (__builtin_expect(!atomic_load_explicit(&g_tsc_calibrated, memory_order_acquire), 0)) {
        pthread_once(&g_tsc_once, ctrace_do_calibrate_tsc);
    }
}

uint64_t ctrace_get_tsc_freq(void) {
    ctrace_calibrate_tsc();
    return g_tsc_freq;
}

uint64_t ctrace_cycles_to_ns(uint64_t cycles) {
    ctrace_calibrate_tsc();
#if (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)) && defined(__SIZEOF_INT128__)
    int64_t delta_c = (int64_t)(cycles - g_tsc_base_cycles);
    if (delta_c >= 0) {
        uint64_t ns_offset = (uint64_t)(((__uint128_t)delta_c * g_tsc_mult) >> 32);
        return g_tsc_base_ns + ns_offset;
    } else {
        uint64_t ns_offset = (uint64_t)(((__uint128_t)(-(uint64_t)delta_c) * g_tsc_mult) >> 32);
        return (g_tsc_base_ns > ns_offset) ? (g_tsc_base_ns - ns_offset) : 0;
    }
#else
    return cycles;
#endif
}

uint64_t ctrace_now_ns(void) {
#if (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)) && defined(__SIZEOF_INT128__)
    return ctrace_cycles_to_ns(ctrace_now_cycles());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

FILE *ctrace_get_stdout_file(void) {
    FILE *f = g_ctrace_out;
    return f ? f : stdout;
}

FILE *ctrace_stdout_acquire(void) {
    pthread_mutex_lock(&g_stdout_mutex);
    if (!g_ctrace_out) {
        g_ctrace_out = stdout;
    }
    flockfile(g_ctrace_out);
    return g_ctrace_out;
}

void ctrace_stdout_release(FILE *f) {
    if (f) {
        funlockfile(f);
    }
    pthread_mutex_unlock(&g_stdout_mutex);
}

#ifndef CTRACE_BACKEND_CUSTOM
// Default implementation of lifecycle for stdout backend
void ctrace_init(const char *target_uri) {
    ctrace_calibrate_tsc();

    pthread_mutex_lock(&g_stdout_mutex);
    if (g_ctrace_out && g_ctrace_owns_file) {
        fclose(g_ctrace_out);
        g_ctrace_out = NULL;
        g_ctrace_owns_file = 0;
    }

    if (!target_uri || strcmp(target_uri, "stdout") == 0 || strcmp(target_uri, "") == 0) {
        g_ctrace_out = stdout;
        g_ctrace_owns_file = 0;
    } else if (strcmp(target_uri, "stderr") == 0) {
        g_ctrace_out = stderr;
        g_ctrace_owns_file = 0;
    } else {
        FILE *f = fopen(target_uri, "w");
        if (f) {
            g_ctrace_out = f;
            g_ctrace_owns_file = 1;
        } else {
            fprintf(stderr, "ctrace warning: Failed to open '%s', falling back to stdout\n", target_uri);
            g_ctrace_out = stdout;
            g_ctrace_owns_file = 0;
        }
    }
    pthread_mutex_unlock(&g_stdout_mutex);
}

void ctrace_flush(void) {
    pthread_mutex_lock(&g_stdout_mutex);
    if (g_ctrace_out) {
        flockfile(g_ctrace_out);
        fflush(g_ctrace_out);
        funlockfile(g_ctrace_out);
    }
    pthread_mutex_unlock(&g_stdout_mutex);
}

void ctrace_shutdown(void) {
    pthread_mutex_lock(&g_stdout_mutex);
    if (g_ctrace_out) {
        flockfile(g_ctrace_out);
        fflush(g_ctrace_out);
        funlockfile(g_ctrace_out);
        if (g_ctrace_owns_file) {
            fclose(g_ctrace_out);
        }
        g_ctrace_out = stdout;
        g_ctrace_owns_file = 0;
    }
    pthread_mutex_unlock(&g_stdout_mutex);
}
#endif
