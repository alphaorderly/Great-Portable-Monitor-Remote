#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t TickType_t;
typedef struct { int value; bool mutex; } TestSemaphore;
typedef TestSemaphore *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define pdPASS 1

#define portTICK_PERIOD_MS 1
