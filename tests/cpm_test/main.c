#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "ctrace/trace.h"

int main(void) {
    ctrace_init("cpm_trace.bin");

    int metric = 42;
    const char *service = "cpm_service";
    TRACE_EVENT(cpm_startup, service, metric);

    ctrace_flush();
    ctrace_shutdown();

    FILE *f = fopen("cpm_trace.bin", "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);

    assert(sz > 0);
    printf("cpm_consumer executed successfully and produced %ld trace bytes!\n", sz);
    return 0;
}
