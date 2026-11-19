#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define pdTRUE 1
