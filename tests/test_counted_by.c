#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "ctrace/trace.h"

// Layer 2: Sub-item with dynamic payload
typedef struct {
    int sensor_id;
    uint16_t num_readings;
    float *readings CTRACE_COUNTED_BY(num_readings);
} SensorData;

// Layer 1: Batch with dynamic sub-items
typedef struct {
    uint32_t batch_id;
    uint16_t num_sensors;
    SensorData *sensors CTRACE_COUNTED_BY(num_sensors);
} TelemetryBatch;

// Single-layer wire packet with Flexible Array Member
typedef struct {
    uint16_t msg_type;
    uint16_t num_items;
    int items[] CTRACE_COUNTED_BY(num_items);
} WireFrame;

int main(void) {
    ctrace_init("test_counted_by.log");

    // Prepare 2-layer hierarchy
    float s1_readings[] = { 1.5f, 2.5f };
    float s2_readings[] = { 10.0f, 20.0f, 30.0f };

    SensorData sensors[2] = {
        { .sensor_id = 101, .num_readings = 2, .readings = s1_readings },
        { .sensor_id = 102, .num_readings = 3, .readings = s2_readings }
    };

    TelemetryBatch batch = {
        .batch_id = 999,
        .num_sensors = 2,
        .sensors = sensors
    };

    // Prepare 1-layer FAM wire frame
    char frame_buf[sizeof(WireFrame) + 3 * sizeof(int)];
    WireFrame *frame = (WireFrame *)frame_buf;
    frame->msg_type = 77;
    frame->num_items = 3;
    frame->items[0] = 100;
    frame->items[1] = 200;
    frame->items[2] = 300;

    // Test 1: 2-layer hierarchy by variable
    TRACE_EVENT(telemetry_batch, batch);

    // Test 2: 2-layer hierarchy by pointer
    const TelemetryBatch *p_batch = &batch;
    TRACE_EVENT(telemetry_ptr, p_batch);

    // Test 3: 1-layer FAM wire frame by pointer
    TRACE_EVENT(wire_frame, frame);

    ctrace_flush();
    ctrace_shutdown();

    // Verify log contents
    FILE *f = fopen("test_counted_by.log", "r");
    assert(f != NULL);

    char buf[4096];
    int saw_batch = 0;
    int saw_ptr = 0;
    int saw_frame = 0;

    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "telemetry_batch") &&
            strstr(buf, "batch={batch_id=999, num_sensors=2") &&
            strstr(buf, "sensors=[{sensor_id=101, num_readings=2, readings=[1.5, 2.5]}") &&
            strstr(buf, "{sensor_id=102, num_readings=3, readings=[10, 20, 30]}]")) {
            saw_batch = 1;
        }

        if (strstr(buf, "telemetry_ptr") &&
            strstr(buf, "p_batch={batch_id=999, num_sensors=2") &&
            strstr(buf, "readings=[1.5, 2.5]") &&
            strstr(buf, "readings=[10, 20, 30]")) {
            saw_ptr = 1;
        }

        if (strstr(buf, "wire_frame") &&
            strstr(buf, "frame={msg_type=77, num_items=3, items=[100, 200, 300]}")) {
            saw_frame = 1;
        }
    }
    fclose(f);

    assert(saw_batch == 1);
    assert(saw_ptr == 1);
    assert(saw_frame == 1);

    printf("test_counted_by passed successfully!\n");
    return 0;
}
