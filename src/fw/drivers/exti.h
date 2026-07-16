/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "board/board.h"

typedef enum {
  ExtiTrigger_Rising,
  ExtiTrigger_Falling,
  ExtiTrigger_RisingFalling
} ExtiTrigger;

typedef void (*ExtiHandlerCallback)(bool *should_context_switch);

void exti_enable(ExtiConfig config);
void exti_disable(ExtiConfig config);

//! Configures the given EXTI and NVIC for the given configuration.
void exti_configure_pin(ExtiConfig cfg, ExtiTrigger trigger, ExtiHandlerCallback cb);

//! sf32lb only: deliver edges that arrived while the SoC was in deep sleep
//! (latched by the AON block, invisible to the GPIO EXTI logic) to their
//! handlers. Called by the SoC sleep code right after each deep-sleep exit.
void exti_handle_deepsleep_wakeups(void);