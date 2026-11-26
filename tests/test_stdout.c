#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    ctrace_init("test_stdout.log");

    int socket_fd = 42;
    size_t len = 100;
    const char *payload_data = "helloworld";
    ctrace_bytes_t payload = TRACE_BYTES(payload_data, 10);
    LogLevel lvl = LOG_WARN;
    AppRequest req = {
        .id = 999,
        .tag = "alpha",
        .sub = { .sub_id = 7, .score = 3.14f }
    };
    bool active = true;

    TRACE_EVENT(net_rx, socket_fd, len, payload);
    TRACE_EVENT(app_req, req.id, req.tag, lvl, active, req);

    ctrace_flush();
    ctrace_shutdown();

    // Verify file contents
    FILE *f = fopen("test_stdout.log", "r");
    assert(f != NULL);
    char buf[1024];
    int line_count = 0;
    int saw_net_rx = 0;
    int saw_app_req = 0;

    while (fgets(buf, sizeof(buf), f)) {
        line_count++;
        if (strstr(buf, "net_rx") && strstr(buf, "socket_fd=42") && strstr(buf, "len=100")) {
            saw_net_rx = 1;
        }
        if (strstr(buf, "app_req") && strstr(buf, "req_id=999") && strstr(buf, "LOG_WARN") && strstr(buf, "active=true")) {
            saw_app_req = 1;
        }
    }
    fclose(f);

    assert(line_count >= 2);
    assert(saw_net_rx == 1);
    assert(saw_app_req == 1);

    printf("test_stdout passed successfully!\n");
    return 0;
}
