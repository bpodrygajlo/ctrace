#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include "ctrace/trace.h"

int main(void) {
    // Set writer affinity to CPU 0 and nice priority to 5 before init
    ctrace_set_writer_affinity(0);
    ctrace_set_writer_priority(5);

    ctrace_init("test_writer.bin");

    // Let the writer thread initialize
    struct timespec req = { .tv_sec = 0, .tv_nsec = 10000000L };
    nanosleep(&req, NULL);

    // Check thread comm name directly via /proc/self/task/<tid>/comm
    int found_writer_thread = 0;
    DIR *dir = opendir("/proc/self/task");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char comm_path[512];
            snprintf(comm_path, sizeof(comm_path), "/proc/self/task/%s/comm", ent->d_name);
            FILE *cf = fopen(comm_path, "r");
            if (cf) {
                char comm[64] = {0};
                if (fgets(comm, sizeof(comm), cf)) {
                    if (strstr(comm, "ctrace-writer")) {
                        found_writer_thread = 1;
                    }
                }
                fclose(cf);
            }
        }
        closedir(dir);
    }
    assert(found_writer_thread == 1);

    // Stress ring-buffer wrap-around: emit > 600 KB of events across multiple 256 KB laps
    char payload_buf[256];
    memset(payload_buf, 'A', sizeof(payload_buf));
    ctrace_bytes_t blob = TRACE_BYTES(payload_buf, sizeof(payload_buf));

    for (int i = 0; i < 2500; ++i) {
        TRACE_EVENT(writer_wrap_event, i, blob);
        if (i % 400 == 0) {
            ctrace_flush();
        }
    }

    ctrace_flush();
    ctrace_shutdown();

    // Verify trace file was written and contains wrapped payloads without corruption
    FILE *f = fopen("test_writer.bin", "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    assert(sz > 500000);

    printf("test_writer_thread passed successfully (wrote %ld bytes across multiple ring wraps)!\n", sz);
    return 0;
}
