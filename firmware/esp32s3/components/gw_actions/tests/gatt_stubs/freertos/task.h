#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
