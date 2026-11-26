#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    ctrace_init("test_bin.bin");

    int socket_fd = 10;
    size_t len = 64;
    const char *data = "binpayload";
    ctrace_bytes_t payload = TRACE_BYTES(data, 10);
    LogLevel lvl = LOG_INFO;
    bool active = false;

    for (int i = 0; i < 50; ++i) {
        TRACE_EVENT(net_rx, socket_fd + i, len, payload);
    }
    TRACE_EVENT(status, lvl, active);

    ctrace_flush();
    ctrace_shutdown();

    printf("test_bin completed emitting events!\n");
    return 0;
}
