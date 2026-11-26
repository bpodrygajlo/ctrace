#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    ctrace_init("test_ctf_trace");

    int socket_fd = 55;
    size_t len = 256;
    const char *payload_data = "ctf_test_payload";
    ctrace_bytes_t payload = TRACE_BYTES(payload_data, 16);
    LogLevel lvl = LOG_WARN;
    bool active = true;
    const char *url = "https://ctf.trace.org/v1";

    for (int i = 0; i < 30; ++i) {
        TRACE_EVENT(net_rx, socket_fd + i, len, payload);
    }
    TRACE_EVENT(api_request, url, lvl, active);

    ctrace_flush();
    ctrace_shutdown();

    printf("test_ctf emitted events to test_ctf_trace!\n");
    return 0;
}
