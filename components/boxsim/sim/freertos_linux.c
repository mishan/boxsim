/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* The linux port runs one core, and asserts on a task pinned to the BOX's
   second: every task to the one there is, linked in by --wrap. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BaseType_t __real_xTaskCreatePinnedToCore (TaskFunction_t, const char *, const uint32_t,
                                           void *, UBaseType_t, TaskHandle_t *,
                                           const BaseType_t);

BaseType_t
__wrap_xTaskCreatePinnedToCore (TaskFunction_t code, const char *name, const uint32_t stack,
                                void *arg, UBaseType_t priority, TaskHandle_t *task,
                                const BaseType_t core)
{
    return __real_xTaskCreatePinnedToCore(code, name, stack, arg, priority, task,
                                          tskNO_AFFINITY);
}
