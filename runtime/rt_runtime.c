#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_ctrace_active_backend = 2; // Default: 2 = bin

// Strong references to ensure linker pulls in backend implementations
void ctrace_init_stdout(const char *target_uri);
void ctrace_init_ringbuf(const char *target_uri);
void ctrace_init_ctf(const char *target_uri);

void ctrace_flush_stdout(void);
void ctrace_flush_ringbuf(void);
void ctrace_flush_ctf(void);

void ctrace_shutdown_stdout(void);
void ctrace_shutdown_ringbuf(void);
void ctrace_shutdown_ctf(void);

void ctrace_init(const char *target_uri) {
    // If a backend is already active, shut it down cleanly before switching
    if (g_ctrace_active_backend != 0) {
        ctrace_shutdown();
    }

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
        if (strcmp(env_b, "stdout") == 0 || strcmp(env_b, "stderr") == 0) backend = 1;
        else if (strcmp(env_b, "bin") == 0) backend = 2;
        else if (strcmp(env_b, "ctf") == 0) backend = 3;
        else if (strcmp(env_b, "sqlite") == 0) {
            fprintf(stderr, "ctrace warning: SQLite backend is not enabled in multi-backend runtime mode; using 'bin'\n");
            backend = 2;
        } else if (strcmp(env_b, "off") == 0 || strcmp(env_b, "none") == 0 || strcmp(env_b, "0") == 0) {
            backend = 0;
        }
    } else if (out_path) {
        if (strcmp(out_path, "stdout") == 0 || strcmp(out_path, "stderr") == 0) backend = 1;
        else if (strstr(out_path, ".bin")) backend = 2;
        else if (strstr(out_path, "ctf") || strstr(out_path, "_ctf")) backend = 3;
        else if (strstr(out_path, ".db") || strstr(out_path, ".sqlite")) {
            fprintf(stderr, "ctrace warning: SQLite backend is not enabled in multi-backend runtime mode; using 'bin'\n");
            backend = 2;
        }
    }

    g_ctrace_active_backend = backend;

    if (backend == 1) {
        ctrace_init_stdout(out_path);
    } else if (backend == 2) {
        ctrace_init_ringbuf(out_path);
    } else if (backend == 3) {
        ctrace_init_ctf(out_path);
    }
}

void ctrace_flush(void) {
    int b = g_ctrace_active_backend;
    if (b == 1) ctrace_flush_stdout();
    else if (b == 2) ctrace_flush_ringbuf();
    else if (b == 3) ctrace_flush_ctf();
}

void ctrace_shutdown(void) {
    int b = g_ctrace_active_backend;
    g_ctrace_active_backend = 0;
    if (b == 1) ctrace_shutdown_stdout();
    else if (b == 2) ctrace_shutdown_ringbuf();
    else if (b == 3) ctrace_shutdown_ctf();
}
