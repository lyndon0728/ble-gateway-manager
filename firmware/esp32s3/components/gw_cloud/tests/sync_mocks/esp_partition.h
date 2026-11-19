#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef struct {size_t size;char label[16];} esp_partition_t;
esp_err_t esp_partition_read(const esp_partition_t *,size_t,void *,size_t);
