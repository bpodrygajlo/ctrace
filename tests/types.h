#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
} LogLevel;

typedef struct {
    int sub_id;
    float score;
} SubInfo;

typedef struct {
    int id;
    const char *tag;
    SubInfo sub;
} AppRequest;
