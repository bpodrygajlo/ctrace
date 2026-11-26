#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    ctrace_init("test_zstd.bin.zst");

    int socket_fd = 20;
    size_t len = 128;
    const char *data = "compressedpayload1234567890";
    ctrace_bytes_t payload = TRACE_BYTES(data, 26);
    LogLevel lvl = LOG_DEBUG;

    for (int i = 0; i < 200; ++i) {
        TRACE_EVENT(net_rx, socket_fd + i, len, payload);
    }
    TRACE_EVENT(heartbeat, lvl);

    ctrace_flush();
    ctrace_shutdown();

    printf("test_zstd completed emitting events!\n");
    return 0;
}
