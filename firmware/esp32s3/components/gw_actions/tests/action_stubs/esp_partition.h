#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {size_t size;char label[16];} esp_partition_t;
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 255
const esp_partition_t *esp_partition_find_first(int,int,const char *);
esp_err_t esp_partition_read(const esp_partition_t *,size_t,void *,size_t);
esp_err_t esp_partition_write(const esp_partition_t *,size_t,const void *,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *,size_t,size_t);
