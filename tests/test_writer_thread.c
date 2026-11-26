#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
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
    usleep(10000); // 10ms

    // Check thread comm name via /proc/self/task/
    // Look through tasks to find "ctrace-writer"
    int found_writer_thread = 0;
    FILE *p = popen("ps -T -o comm,policy,pri -p $(pgrep -n test_writer_thread || echo $$)", "r");
    if (p) {
        char line[256];
        while (fgets(line, sizeof(line), p)) {
            if (strstr(line, "ctrace-writer")) {
                found_writer_thread = 1;
                // Verify scheduling policy is TS/OTHER (not FIFO/RR)
                assert(!strstr(line, "FF") && !strstr(line, "RR"));
            }
        }
        pclose(p);
    }

    // Write a trace event
    int x = 42;
    TRACE_EVENT(writer_test_event, x);

    ctrace_flush();
    ctrace_shutdown();

    // Verify trace file was written
    FILE *f = fopen("test_writer.bin", "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    assert(sz > 0);

    // If ps could read threads, verify we saw ctrace-writer
    if (found_writer_thread) {
        printf("Verified ctrace-writer thread name and non-RT scheduling policy!\n");
    }

    printf("test_writer_thread passed successfully!\n");
    return 0;
}
