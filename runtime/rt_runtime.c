#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_ctrace_active_backend = 2; // Default: 2 = bin

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_init_stdout(const char *target_uri);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_init_ringbuf(const char *target_uri);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_init_ctf(const char *target_uri);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_init_sqlite(const char *target_uri);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_flush_stdout(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_flush_ringbuf(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_flush_ctf(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_flush_sqlite(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_shutdown_stdout(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_shutdown_ringbuf(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_shutdown_ctf(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ctrace_shutdown_sqlite(void);

void ctrace_init(const char *target_uri) {
    const char *out_path = target_uri;
    if (!out_path || strlen(out_path) == 0) {
        const char *env_p = getenv("CTRACE_OUTPUT_PATH");
        if (!env_p) env_p = getenv("CTRACE_FILE");
        if (env_p && strlen(env_p) > 0) {
            out_path = env_p;
        }
    }

    const char *env_b = getenv("CTRACE_BACKEND");
    int backend = 2; // Default: bin

    if (env_b && strlen(env_b) > 0) {
        if (strcmp(env_b, "stdout") == 0) backend = 1;
        else if (strcmp(env_b, "bin") == 0) backend = 2;
        else if (strcmp(env_b, "ctf") == 0) backend = 3;
        else if (strcmp(env_b, "sqlite") == 0) backend = 4;
        else if (strcmp(env_b, "off") == 0 || strcmp(env_b, "none") == 0 || strcmp(env_b, "0") == 0) backend = 0;
    } else if (out_path) {
        if (strstr(out_path, ".db") || strstr(out_path, ".sqlite")) backend = 4;
        else if (strstr(out_path, "ctf") || strstr(out_path, "_ctf")) backend = 3;
        else if (strstr(out_path, ".bin")) backend = 2;
        else if (strcmp(out_path, "stdout") == 0) backend = 1;
    }

    g_ctrace_active_backend = backend;

    if (backend == 1 && ctrace_init_stdout) {
        ctrace_init_stdout(out_path);
    } else if (backend == 2 && ctrace_init_ringbuf) {
        ctrace_init_ringbuf(out_path);
    } else if (backend == 3 && ctrace_init_ctf) {
        ctrace_init_ctf(out_path);
    } else if (backend == 4 && ctrace_init_sqlite) {
        ctrace_init_sqlite(out_path);
    }
}

void ctrace_flush(void) {
    if (g_ctrace_active_backend == 1 && ctrace_flush_stdout) ctrace_flush_stdout();
    else if (g_ctrace_active_backend == 2 && ctrace_flush_ringbuf) ctrace_flush_ringbuf();
    else if (g_ctrace_active_backend == 3 && ctrace_flush_ctf) ctrace_flush_ctf();
    else if (g_ctrace_active_backend == 4 && ctrace_flush_sqlite) ctrace_flush_sqlite();
}

void ctrace_shutdown(void) {
    if (g_ctrace_active_backend == 1 && ctrace_shutdown_stdout) ctrace_shutdown_stdout();
    else if (g_ctrace_active_backend == 2 && ctrace_shutdown_ringbuf) ctrace_shutdown_ringbuf();
    else if (g_ctrace_active_backend == 3 && ctrace_shutdown_ctf) ctrace_shutdown_ctf();
    else if (g_ctrace_active_backend == 4 && ctrace_shutdown_sqlite) ctrace_shutdown_sqlite();
    g_ctrace_active_backend = 0;
}
