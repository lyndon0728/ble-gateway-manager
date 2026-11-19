#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_DATA 1
typedef struct {uint32_t size;bool encrypted;} esp_partition_t;
const esp_partition_t *esp_partition_find_first(int,int,const char *);
esp_err_t esp_partition_read(const esp_partition_t *,size_t,void *,size_t);
esp_err_t esp_partition_read_raw(const esp_partition_t *,size_t,void *,size_t);
esp_err_t esp_partition_write(const esp_partition_t *,size_t,const void *,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *,size_t,size_t);
