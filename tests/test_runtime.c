#include "ctrace/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    ctrace_init(NULL); // Picks up CTRACE_BACKEND and CTRACE_OUTPUT_PATH from env

    int a = 123;
    const char *msg = "hello_runtime";
    double pi = 3.14159;

    TRACE_EVENT(runtime_event, a, msg, pi);

    ctrace_flush();
    ctrace_shutdown();
    return 0;
}
