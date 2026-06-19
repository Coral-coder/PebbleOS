/* SPDX-FileCopyrightText: 2025 SiFli Technologies(Nanjing) Co., Ltd */
/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/exti.h"

#include <stdbool.h>

#include "board/board.h"
#include "kernel/events.h"
#include "mcu/interrupts.h"
#include "system/logging.h"
#include "system/passert.h"

#include <bf0_hal.h>

PBL_LOG_MODULE_DEFINE(driver_exti_sf32lb, CONFIG_DRIVER_EXTI_LOG_LEVEL);

#define EXTI_MAX_GPIO1_PIN_NUM 16

#ifdef CONFIG_SOC_SF32LB52
// SF32LB52 maps AON wakeup PINn to PAn, with PIN0 = PA24.
#define SF32LB52_AON_WAKEUP_GPIO_PIN_FIRST 24U
#define SF32LB52_AON_WAKEUP_GPIO_PIN_LAST 44U
#endif

typedef struct {
  uint32_t gpio_pin;
  ExtiTrigger trigger;
  ExtiHandlerCallback callback;
} ExtiHandlerConfig_t;

static ExtiHandlerConfig_t s_exti_gpio1_handler_configs[EXTI_MAX_GPIO1_PIN_NUM];
static bool s_should_context_switch;

static GPIO_TypeDef *prv_gpio_get_instance(GPIO_TypeDef *hgpio, uint16_t gpio_pin,
                                           uint16_t *offset) {
  uint16_t inst_idx;
  GPIO_TypeDef *gpiox;

  HAL_ASSERT(gpio_pin < GPIO1_PIN_NUM);

  if (gpio_pin >= GPIO1_PIN_NUM) {
    return (GPIO_TypeDef *)NULL;
  }

  // There are many groups of similar registers in the GPIO, and because of register length limitations, up to 32 gpio can be operated in each group.
  inst_idx = gpio_pin >> 5;
  *offset = gpio_pin & 31;

  gpiox = (GPIO_TypeDef *)hgpio + inst_idx;

  return gpiox;
}

#ifdef CONFIG_SOC_SF32LB52
static int prv_find_handler_index(uint8_t gpio_pin) {
  for (uint8_t index = 0; index < EXTI_MAX_GPIO1_PIN_NUM; index++) {
    if (s_exti_gpio1_handler_configs[index].callback != NULL &&
        s_exti_gpio1_handler_configs[index].gpio_pin == gpio_pin) {
      return index;
    }
  }
  return -1;
}

static bool prv_gpio_pin_to_aon_src(uint8_t gpio_pin, HPAON_WakeupSrcTypeDef *src_out) {
  if (gpio_pin < SF32LB52_AON_WAKEUP_GPIO_PIN_FIRST ||
      gpio_pin > SF32LB52_AON_WAKEUP_GPIO_PIN_LAST) {
    return false;
  }

  *src_out = (HPAON_WakeupSrcTypeDef)(HPAON_WAKEUP_SRC_PIN0 +
                                      (gpio_pin - SF32LB52_AON_WAKEUP_GPIO_PIN_FIRST));
  return true;
}

static AON_PinModeTypeDef prv_exti_trigger_to_aon_mode(ExtiTrigger trigger) {
  switch (trigger) {
    case ExtiTrigger_Rising:
      return AON_PIN_MODE_POS_EDGE;
    case ExtiTrigger_Falling:
      return AON_PIN_MODE_NEG_EDGE;
    case ExtiTrigger_RisingFalling:
      return AON_PIN_MODE_DOUBLE_EDGE;
    default:
      return AON_PIN_MODE_POS_EDGE;
  }
}

static void prv_set_aon_pin_wakeup(uint8_t gpio_pin, ExtiTrigger trigger, bool enable) {
  HPAON_WakeupSrcTypeDef src;
  if (!prv_gpio_pin_to_aon_src(gpio_pin, &src)) {
    return;
  }

  if (enable) {
    HAL_HPAON_EnableWakeupSrc(src, prv_exti_trigger_to_aon_mode(trigger));
  } else {
    HAL_HPAON_DisableWakeupSrc(src);
  }
}
#endif

static void prv_insert_handler(GPIO_TypeDef *hgpio, uint8_t gpio_pin, ExtiTrigger trigger,
                               ExtiHandlerCallback cb) {
  // Find the handler index for this pin
  uint8_t index = 0;
  while (index < EXTI_MAX_GPIO1_PIN_NUM &&
         s_exti_gpio1_handler_configs[index].callback != NULL) {
    index++;
  }
  if (index >= EXTI_MAX_GPIO1_PIN_NUM) {
    // No available slot
    return;
  }
  // Store the callback and index
  s_exti_gpio1_handler_configs[index].gpio_pin = gpio_pin;
  s_exti_gpio1_handler_configs[index].trigger = trigger;
  s_exti_gpio1_handler_configs[index].callback = cb;
}

void exti_configure_pin(ExtiConfig cfg, ExtiTrigger trigger, ExtiHandlerCallback cb) {
  GPIO_InitTypeDef init;
  int flags;

  init.Pin = cfg.gpio_pin;
  init.Pull = GPIO_NOPULL;

  switch (cfg.pull) {
    case GPIO_PuPd_UP:
      flags = PIN_PULLUP;
      break;
    case GPIO_PuPd_DOWN:
      flags = PIN_PULLDOWN;
      break;
    default:
      flags = PIN_NOPULL;
      break;
  }

  switch (trigger) {
    case ExtiTrigger_Rising:
      init.Mode = GPIO_MODE_IT_RISING;
      break;
    case ExtiTrigger_Falling:
      init.Mode = GPIO_MODE_IT_FALLING;
      break;
    case ExtiTrigger_RisingFalling:
      init.Mode = GPIO_MODE_IT_RISING_FALLING;
      break;
  }

  HAL_NVIC_DisableIRQ(GPIO1_IRQn);

  HAL_PIN_Set(PAD_PA00 + cfg.gpio_pin, GPIO_A0 + cfg.gpio_pin, flags, 1);
  HAL_GPIO_Init(cfg.peripheral, &init);

  prv_insert_handler(cfg.peripheral, cfg.gpio_pin, trigger, cb);

  HAL_NVIC_SetPriority(GPIO1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(GPIO1_IRQn);
}

void exti_enable(ExtiConfig cfg) {
  uint16_t offset;
  GPIO_TypeDef *gpiox = prv_gpio_get_instance(cfg.peripheral, cfg.gpio_pin, &offset);
  gpiox->IESR = (1 << offset);

#ifdef CONFIG_SOC_SF32LB52
  const int index = prv_find_handler_index(cfg.gpio_pin);
  if (index >= 0) {
    prv_set_aon_pin_wakeup(cfg.gpio_pin, s_exti_gpio1_handler_configs[index].trigger, true);
  }
#endif
}

void exti_disable(ExtiConfig cfg) {
  uint16_t offset;
  GPIO_TypeDef *gpiox = prv_gpio_get_instance(cfg.peripheral, cfg.gpio_pin, &offset);
  gpiox->IECR = (1 << offset);
  gpiox->ISR = (1 << offset);

#ifdef CONFIG_SOC_SF32LB52
  prv_set_aon_pin_wakeup(cfg.gpio_pin, ExtiTrigger_Rising, false);
#endif
}

void HAL_GPIO_EXTI_Callback(GPIO_TypeDef *hgpio, uint16_t GPIO_Pin) {
  for (uint8_t index = 0; index < EXTI_MAX_GPIO1_PIN_NUM; index++) {
    if (s_exti_gpio1_handler_configs[index].callback != NULL &&
        s_exti_gpio1_handler_configs[index].gpio_pin == GPIO_Pin) {
      bool should_context_switch = false;

      s_exti_gpio1_handler_configs[index].callback(&should_context_switch);
      s_should_context_switch |= should_context_switch;
      return;
    }
  }

  PBL_LOG_WRN("No handler found for GPIO pin %u", GPIO_Pin);
}

void GPIO1_IRQHandler(void) {
  s_should_context_switch = false;
  HAL_GPIO_IRQHandler(hwp_gpio1);
  portEND_SWITCHING_ISR(s_should_context_switch);
}

bool exti_dispatch_aon_pin_wakes(uint32_t pin_wsr_mask) {
  if (pin_wsr_mask == 0U) {
    return false;
  }

  s_should_context_switch = false;

  uint32_t pin_wsr = pin_wsr_mask >> HPSYS_AON_WSR_PIN0_Pos;
  for (uint32_t i = 0; (i < HPSYS_AON_WSR_PIN_NUM) && pin_wsr; i++) {
    if (pin_wsr & 1U) {
      uint16_t gpio_pin;
      GPIO_TypeDef *gpio = HAL_HPAON_QueryWakeupGpioPin((uint8_t)i, &gpio_pin);
      if (gpio != NULL) {
        HAL_GPIO_ClearPinInterrupt(gpio, gpio_pin);
        HAL_GPIO_EXTI_Callback(gpio, gpio_pin);
      }
    }
    pin_wsr >>= 1;
  }

  return s_should_context_switch;
}
