#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    double lat;
    double lon;
} GeoLocation;

typedef enum {
    DEV_OFFLINE = 0,
    DEV_ONLINE  = 1,
    DEV_STANDBY = 2
} DevState;

typedef struct {
    int code;
    DevState state;
} DeviceStatus;

typedef struct {
    int sensor_id;
    float readings[3];
    GeoLocation loc;
    DeviceStatus status;
} Sensor;

typedef struct {
    int device_id;
    const char *model;
    Sensor primary;
    Sensor backup;
} Device;

#include "ctrace/trace.h"

int main(void) {
    ctrace_init("test_complex.log");

    Device dev = {
        .device_id = 101,
        .model = "SensorHub-X",
        .primary = {
            .sensor_id = 1,
            .readings = { 1.1f, 2.2f, 3.3f },
            .loc = { .lat = 37.7749, .lon = -122.4194 },
            .status = { .code = 200, .state = DEV_ONLINE }
        },
        .backup = {
            .sensor_id = 2,
            .readings = { 0.1f, 0.2f, 0.3f },
            .loc = { .lat = 37.7750, .lon = -122.4200 },
            .status = { .code = 503, .state = DEV_STANDBY }
        }
    };

    const Device *p_dev = &dev;
    const Device *null_dev = NULL;

    // 1. Entire struct by value
    TRACE_EVENT(device_val, dev);

    // 2. Entire struct by pointer
    TRACE_EVENT(device_ptr, p_dev);

    // 3. Entire struct by address
    TRACE_EVENT(device_addr, &dev);

    // 4. NULL struct pointer
    TRACE_EVENT(device_null, null_dev);

    ctrace_flush();
    ctrace_shutdown();

    // Verify log file output
    FILE *f = fopen("test_complex.log", "r");
    assert(f != NULL);

    char buf[4096];
    int saw_val = 0;
    int saw_ptr = 0;
    int saw_addr = 0;
    int saw_null = 0;

    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "device_val") &&
            strstr(buf, "dev={device_id=101") &&
            strstr(buf, "model=\"SensorHub-X\"") &&
            strstr(buf, "primary={sensor_id=1") &&
            strstr(buf, "readings_0=1.1") &&
            strstr(buf, "readings_1=2.2") &&
            strstr(buf, "readings_2=3.3") &&
            strstr(buf, "loc={lat=37.7749") &&
            strstr(buf, "status={code=200, state=DEV_ONLINE}") &&
            strstr(buf, "backup={sensor_id=2") &&
            strstr(buf, "status={code=503, state=DEV_STANDBY}")) {
            saw_val = 1;
        }

        if (strstr(buf, "device_ptr") &&
            strstr(buf, "p_dev={device_id=101") &&
            strstr(buf, "model=\"SensorHub-X\"") &&
            strstr(buf, "state=DEV_ONLINE")) {
            saw_ptr = 1;
        }

        if (strstr(buf, "device_addr") &&
            strstr(buf, "dev={device_id=101") &&
            strstr(buf, "state=DEV_STANDBY")) {
            saw_addr = 1;
        }

        if (strstr(buf, "device_null") && strstr(buf, "null_dev=(null)")) {
            saw_null = 1;
        }
    }
    fclose(f);

    assert(saw_val == 1);
    assert(saw_ptr == 1);
    assert(saw_addr == 1);
    assert(saw_null == 1);

    printf("test_complex passed successfully!\n");
    return 0;
}
