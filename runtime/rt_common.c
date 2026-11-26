#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *g_ctrace_out = NULL;
static int g_ctrace_owns_file = 0;

static uint64_t g_tsc_freq = 0;
static uint64_t g_tsc_base_cycles = 0;
static uint64_t g_tsc_base_ns = 0;
static uint64_t g_tsc_mult = 0;
static int g_tsc_calibrated = 0;

void ctrace_calibrate_tsc(void) {
    if (g_tsc_calibrated) return;

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    uint64_t c0 = ctrace_now_cycles();

    // 5ms calibration window
    usleep(5000);

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    uint64_t c1 = ctrace_now_cycles();

    uint64_t delta_ns = (ts1.tv_sec - ts0.tv_sec) * 1000000000ULL + (ts1.tv_nsec - ts0.tv_nsec);
    uint64_t delta_c = c1 - c0;
    if (delta_ns == 0) delta_ns = 1;

    g_tsc_freq = (delta_c * 1000000000ULL) / delta_ns;
    if (g_tsc_freq == 0) g_tsc_freq = 2000000000ULL; // fallback 2GHz

    g_tsc_base_cycles = c0;
    g_tsc_base_ns = (uint64_t)ts0.tv_sec * 1000000000ULL + (uint64_t)ts0.tv_nsec;

    // Fixed point factor: mult = (1e9 << 32) / freq
    g_tsc_mult = ((uint64_t)1000000000ULL << 32) / g_tsc_freq;
    g_tsc_calibrated = 1;
}

__attribute__((constructor)) static void ctrace_auto_init_tsc(void) {
    ctrace_calibrate_tsc();
}

uint64_t ctrace_get_tsc_freq(void) {
    if (!g_tsc_calibrated) ctrace_calibrate_tsc();
    return g_tsc_freq;
}

uint64_t ctrace_cycles_to_ns(uint64_t cycles) {
    if (__builtin_expect(!g_tsc_calibrated, 0)) {
        ctrace_calibrate_tsc();
    }
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
    int64_t delta_c = (int64_t)(cycles - g_tsc_base_cycles);
    if (delta_c >= 0) {
        uint64_t ns_offset = (uint64_t)(((__uint128_t)delta_c * g_tsc_mult) >> 32);
        return g_tsc_base_ns + ns_offset;
    } else {
        uint64_t ns_offset = (uint64_t)(((__uint128_t)(-delta_c) * g_tsc_mult) >> 32);
        return (g_tsc_base_ns > ns_offset) ? (g_tsc_base_ns - ns_offset) : 0;
    }
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t ctrace_now_ns(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
    return ctrace_cycles_to_ns(ctrace_now_cycles());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

FILE *ctrace_get_stdout_file(void) {
    if (!g_ctrace_out) {
        g_ctrace_out = stdout;
    }
    return g_ctrace_out;
}

#ifndef CTRACE_BACKEND_CUSTOM
// Default implementation of lifecycle for stdout backend
void ctrace_init(const char *target_uri) {
    if (!g_tsc_calibrated) {
        ctrace_calibrate_tsc();
    }

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
            g_ctrace_out = stdout;
            g_ctrace_owns_file = 0;
        }
    }
}

void ctrace_flush(void) {
    if (g_ctrace_out) {
        flockfile(g_ctrace_out);
        fflush(g_ctrace_out);
        funlockfile(g_ctrace_out);
    }
}

void ctrace_shutdown(void) {
    ctrace_flush();
    if (g_ctrace_out && g_ctrace_owns_file) {
        fclose(g_ctrace_out);
        g_ctrace_out = NULL;
        g_ctrace_owns_file = 0;
    }
}
#endif
