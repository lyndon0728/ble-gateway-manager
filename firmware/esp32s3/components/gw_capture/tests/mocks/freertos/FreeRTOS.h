#pragma once
#include <stdint.h>
typedef uint64_t TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT64_MAX
#define pdMS_TO_TICKS(v) (v)
