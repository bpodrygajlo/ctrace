#include "types.h"
#include "ctrace/trace.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

typedef struct {
    int sensor_id;
    uint16_t num_readings;
    float *readings CTRACE_COUNTED_BY(num_readings);
} BinSensor;

typedef struct {
    uint32_t batch_id;
    uint16_t num_sensors;
    BinSensor *sensors CTRACE_COUNTED_BY(num_sensors);
} BinBatch;

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

    float r1[] = { 1.5f, 2.5f };
    float r2[] = { 10.0f, 20.0f, 30.0f };
    BinSensor sensors[2] = {
        { .sensor_id = 101, .num_readings = 2, .readings = r1 },
        { .sensor_id = 102, .num_readings = 3, .readings = r2 }
    };
    BinBatch batch = { .batch_id = 999, .num_sensors = 2, .sensors = sensors };
    TRACE_EVENT(bin_telemetry, batch);

    ctrace_flush();
    ctrace_shutdown();

    printf("test_bin completed emitting events!\n");
    return 0;
}
