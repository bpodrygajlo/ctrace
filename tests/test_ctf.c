#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* Counted struct array whose element carries bools, an enum and a bytes payload */
typedef struct {
    uint8_t kind;
    bool f1;
    bool f2;
    LogLevel lvl;
    ctrace_bytes_t data;
} CtfItem;
typedef struct {
    uint16_t n;
    CtfItem *items CTRACE_COUNTED_BY(n);
} CtfList;

int main(void) {
    ctrace_init("test_ctf_trace");

    int socket_fd = 55;
    size_t len = 256;
    const char *payload_data = "ctf_test_payload";
    ctrace_bytes_t payload = TRACE_BYTES(payload_data, 16);
    LogLevel lvl = LOG_WARN;
    bool active = true;
    const char *url = "https://ctf.trace.org/v1";
    float latency_ms = 1.25f;
    double throughput_gbps = 9.875;

    for (int i = 0; i < 30; ++i) {
        TRACE_EVENT(net_rx, socket_fd + i, len, payload);
    }
    TRACE_EVENT(api_request, url, lvl, active, latency_ms, throughput_gbps);

    CtfItem items[2] = {
        { .kind = 1, .f1 = true, .f2 = false, .lvl = LOG_INFO, .data = TRACE_BYTES("ab", 2) },
        { .kind = 2, .f1 = false, .f2 = true, .lvl = LOG_ERROR, .data = TRACE_BYTES("cd", 2) }
    };
    CtfList list = { .n = 2, .items = items };
    int marker = 4242;
    TRACE_EVENT(ctf_list_event, list, marker);

    // Field names that are TSDL keywords
    int stream = 1;
    int event = 2;
    TRACE_EVENT(ctf_reserved, stream, event);

    ctrace_flush();
    ctrace_shutdown();

    printf("test_ctf emitted events to test_ctf_trace!\n");
    return 0;
}
