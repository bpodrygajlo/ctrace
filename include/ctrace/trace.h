#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ctrace/ctrace_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

void ctrace_init(const char *target_uri); // e.g., NULL (stdout), "trace.bin", or ":memory:"
void ctrace_flush(void);
void ctrace_shutdown(void);

// Background writer thread controls (bin backend)
void ctrace_set_writer_affinity(int cpu_core);
void ctrace_set_writer_priority(int nice_val);

// Hardware TSC & low-latency cycle timestamping
static inline uint64_t ctrace_now_cycles(void) {
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__i386__) || defined(_M_IX86)
    uint64_t val;
    __asm__ volatile("rdtsc" : "=A"(val));
    return val;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t ctrace_now_ns(void);
uint64_t ctrace_get_tsc_freq(void);
uint64_t ctrace_cycles_to_ns(uint64_t cycles);

#if defined(CTRACE_BACKEND_SQLITE) || defined(CTRACE_ENABLE_SQL_API)
int ctrace_sql_exec(const char *sql, int (*cb)(void*, int, char**, char**), void *arg);
int ctrace_sql_snapshot(const char *dest_path); // Wraps VACUUM INTO
#endif

#ifdef CTRACE_AST_SCAN
void __ctrace_hook(const char *event_name, ...);
#if defined(__GNUC__) || defined(__clang__)
#define TRACE_EVENT(name, ...) __ctrace_hook(#name, ##__VA_ARGS__)
#else
#define TRACE_EVENT(name, ...) __ctrace_hook(#name, __VA_ARGS__)
#endif
#elif !defined(CTRACE_RUNTIME)
#include "trace_generated.h"
#define TRACE_EVENT(name, ...) __ctrace_emit_##name(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif
