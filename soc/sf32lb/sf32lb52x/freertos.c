/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <inttypes.h>
#include <stdio.h>

#include "board/board.h"
#include "console/prompt.h"
#include "drivers/exti.h"
#include "drivers/flash.h"
#include "drivers/mcu.h"
#include "drivers/rtc.h"
#include "drivers/sf32lb52/rc10k.h"
#include "drivers/task_watchdog.h"
#include "kernel/util/idle.h"
#include "pbl/os/tick.h"
#include "pbl/services/analytics/analytics.h"
#include "pbl/soc/sf32lb/sleep.h"
#include "pbl/util/math.h"

#include <bf0_hal.h>

#include <ipc_queue.h>

#include "FreeRTOS.h"
#include "task.h"

// HAL tick counter (milliseconds) - used by HAL timeout functions
extern __IO uint32_t uwTick;

static LPTIM_HandleTypeDef s_lptim = {
    .Instance = LPTIM1,
};

// CPU analytics tracking
typedef enum {
  SleepTypeNone = 0,
  SleepTypeWfi,
  SleepTypeDeepWfi,
} SleepType;

static volatile SleepType s_last_sleep_type;
static RtcTicks s_analytics_wfi_ticks;
static RtcTicks s_analytics_deepwfi_ticks;
static RtcTicks s_analytics_deepsleep_ticks;
static RtcTicks s_last_ticks;
static uint32_t s_analytics_ipc_not_idle_count;
static bool s_force_wfi;

// Monotonic (never-reset) counters that back the on-watch Deep Sleep Stats
// screen. Distinct from the s_analytics_* counters above, which reset on each
// analytics collection.
static RtcTicks s_total_wfi_ticks;
static RtcTicks s_total_deepwfi_ticks;
static RtcTicks s_total_deepsleep_ticks;

//! Early wake-up ticks (to avoid over-sleeping due to wake-up latency)
static const uint32_t EARLY_WAKEUP_TICKS = 4;
//! Minimum ticks to enter deep sleep
static const uint32_t MIN_DEEPSLEEP_TICKS = RTC_TICKS_HZ / 20;
//! Maximum LPTIM counter value (24-bit)
static const uint32_t MAX_LPTIM_CNT = 0xFFFFFFUL;

static uint32_t s_iser_bak[16];

static void prv_wdt_feed(uint16_t elapsed_ticks) {
  static uint32_t wdt_feed_ticks;

  wdt_feed_ticks += elapsed_ticks;
  if (wdt_feed_ticks >= (RTC_TICKS_HZ / (1000 / TASK_WATCHDOG_FEED_PERIOD_MS))) {
    wdt_feed_ticks = 0U;
    task_watchdog_feed();
  }
}

static void prv_save_iser(void) {
  uint32_t i;
  for (i = 0; i < 16; i++) {
    s_iser_bak[i] = NVIC->ISER[i];
    NVIC->ICER[i] = 0xFFFFFFFF;
    __DSB();
    __ISB();
  }
}

static void prv_restore_iser(void) {
  uint32_t i;
  for (i = 0; i < 16; i++) {
    __COMPILER_BARRIER();
    NVIC->ISER[i] = s_iser_bak[i];
    __COMPILER_BARRIER();
  }
}

static inline void prv_enter_wfi(void) {
  s_last_sleep_type = SleepTypeWfi;
  __WFI();
}

static void prv_enter_deepwfi(void) {
  s_last_sleep_type = SleepTypeDeepWfi;

  flash_power_down_for_stop_mode();

  __DSB();
  __ISB();

  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
  __WFI();
  SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
}

static void prv_enter_deepslep(void) {
  QSPIPortState *flash_state;
  uint32_t dll1_freq = 0UL;
  int clk_src;

  flash_state = QSPI_FLASH->qspi->state;

  prv_save_iser();

  HAL_FLASH_NOP_CMD(&flash_state->ctx.handle);
  HAL_FLASH_DEEP_PWRDOWN(&flash_state->ctx.handle);
  HAL_Delay_us(flash_state->t_enter_deep_us);

  NVIC_EnableIRQ(AON_IRQn);

  clk_src = HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_SYS);
  if (clk_src == RCC_SYSCLK_DLL1) {
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_HRC48);
    dll1_freq = HAL_RCC_HCPU_GetDLL1Freq();
    HAL_RCC_HCPU_DisableDLL1();
  }

  HAL_HPAON_DISABLE_PAD();
  HAL_HPAON_DISABLE_VHP();

  HAL_HPAON_CLEAR_HP_ACTIVE();
  HAL_HPAON_SET_POWER_MODE(AON_PMR_DEEP_SLEEP);

  __WFI();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();

  HAL_HPAON_ENABLE_PAD();
  HAL_HPAON_ENABLE_VHP();

  HAL_HPAON_SET_HP_ACTIVE();
  HAL_HPAON_CLEAR_POWER_MODE();

  // Wait for HXT48 to be ready
  if (dll1_freq != 0UL) {
    while (0 == (hwp_hpsys_aon->ACR & HPSYS_AON_ACR_HXT48_RDY)) {
      __NOP();
    }
  }

  if (clk_src == RCC_SYSCLK_DLL1) {
    HAL_RCC_HCPU_EnableDLL1(dll1_freq);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_DLL1);
    HAL_Delay_us(0);
  }

  HAL_FLASH_RELEASE_DPD(&flash_state->ctx.handle);
  HAL_Delay_us(flash_state->t_exit_deep_us);

  prv_restore_iser();
}

static uint32_t prv_calc_elapsed_ticks(uint32_t gtimer_cyc) {
  static uint16_t s_err_milli_ticks;
  uint32_t elapsed_milli_ticks;
  uint32_t elapsed_ticks;

  elapsed_milli_ticks = rc10k_cyc_to_milli_ticks(gtimer_cyc);
  elapsed_ticks = elapsed_milli_ticks / 1000U;
  s_err_milli_ticks += (elapsed_milli_ticks % 1000U);
  if (s_err_milli_ticks >= 1000U) {
    elapsed_ticks++;
    s_err_milli_ticks -= 1000U;
  }

  return elapsed_ticks;
}

void vPortSuppressTicksAndSleep(TickType_t xExpectedIdleTime) {
  // On the early-out paths the FreeRTOS idle task has no WFI of its own, so a
  // bare `return` leaves the core spinning at full clock until the next tick.
  // Halt with WFI instead; it wakes on any pending interrupt (SysTick within a
  // tick, or the IPC/peripheral IRQ we are waiting on).
  if (!idle_is_allowed()) {
    __WFI();
    return;
  }

  if (!ipc_queue_check_idle()) {
    s_analytics_ipc_not_idle_count++;
    // Pending HCPU<->LCPU (BLE) traffic: halt until the next interrupt rather
    // than busy-spinning the idle task while the IPC queue drains.
    __WFI();
    return;
  }

  __disable_irq();

  if (eTaskConfirmSleepModeStatus() != eAbortSleep) {
    SocSf32lbSleepLevel max_level = soc_sf32lb_sleep_max_level();

    // Deep sleep needs a minimum idle window.
    if (xExpectedIdleTime < MIN_DEEPSLEEP_TICKS) {
      max_level = MIN(max_level, SOC_SF32LB_DEEPWFI);
    }

    // The debug flag forces plain WFI (deep WFI and deep sleep disabled).
    if (s_force_wfi) {
      max_level = MIN(max_level, SOC_SF32LB_WFI);
    }

    switch (max_level) {
      case SOC_SF32LB_WFI:
        prv_enter_wfi();
        break;
      case SOC_SF32LB_DEEPWFI:
        prv_enter_deepwfi();
        break;
      case SOC_SF32LB_DEEPSLEEP: {
        uint32_t gtimer_start;
        uint32_t gtimer_stop;
        uint32_t gtimer_delta;
        uint32_t sleep_ticks;
        uint32_t lptim_ticks;
        uint32_t elapsed_ticks;

        // stop systick
        SysTick->CTRL &= ~(SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);
        SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

        // configure LPTIM to wake us up after expected idle time
        sleep_ticks = xExpectedIdleTime - EARLY_WAKEUP_TICKS;
        lptim_ticks = MIN(sleep_ticks * rc10k_get_freq_hz() / RTC_TICKS_HZ,
                          MAX_LPTIM_CNT);
        HAL_LPTIM_Counter_Start_IT(&s_lptim, lptim_ticks);

        gtimer_start = HAL_GTIMER_READ();

        prv_enter_deepslep();

        // NOTE: GTIMER needs at least 1 LP clock cycle to be updated after sleep,
        // so spin until we see a change
        do {
          gtimer_stop = HAL_GTIMER_READ();
        } while (gtimer_stop == gtimer_start);

        if (gtimer_stop < gtimer_start) {
          gtimer_delta = (UINT32_MAX - gtimer_start) + gtimer_stop + 1UL;
        } else {
          gtimer_delta = gtimer_stop - gtimer_start;
        }

        elapsed_ticks = prv_calc_elapsed_ticks(gtimer_delta);

        vTaskStepTick(elapsed_ticks);

        // increment HAL tick counter by elapsed ticks
        uwTick += elapsed_ticks;

        prv_wdt_feed(elapsed_ticks);

        // Force RTC synchronization of shadow registers
        hwp_rtc->ISR &= RTC_RSF_MASK;

        // Track deep sleep time for analytics
        s_analytics_deepsleep_ticks += elapsed_ticks;
        s_total_deepsleep_ticks += elapsed_ticks;

        // stop LPTIM
        HAL_LPTIM_Counter_Stop_IT(&s_lptim);

        // enable systick
        SysTick->CTRL |= (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);

        // Deliver GPIO edges that arrived during deep sleep: the pads were
        // off, so they only latched in the AON wake-status register and the
        // GPIO EXTI never fired. Without this a touch INT pulse during deep
        // sleep is lost and the controller wedges until its watchdog.
        exti_handle_deepsleep_wakeups();
        break;
      }
      default:
        break;
    }
  }

  __enable_irq();
}

bool vPortEnableTimer() {
  HAL_LPTIM_InitDefault(&s_lptim);
  HAL_LPTIM_Init(&s_lptim);

  // configure SYSTICK
  // - use HXT48 as TICK reference clock
  // - divide TICK reference clock to 1.92MHz (48MHz / 25) (TICK_CLK)
  // - enable TICK_CLK as SYSTICK clock source
  // - configure SYSTICK to generate interrupt at RTC_TICKS_HZ rate
  //
  // HXT48│\                        ┌────────────────┐
  //  ────┼ \                       │         SYSTICK│
  // HRC48│  │  ┌───────┐  TICK_CLK ││\              │
  //  ────┼  ├──│TICKDIV├───────────┼┤ │   ┌──────┐  │
  //   ...│  │  └───────┘.         ┌┼┤ ├───│RELOAD├──┤
  //      │ /                      │││/    └──────┘  │
  //      │/                       ││                │
  //           HCLK                ││                │
  //          ─────────────────────┘└────────────────┘

  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_HP_TICK, RCC_CLK_TICK_HXT48);
  // delay to avoid systick config failure (undocumented silicon issue)
  HAL_Delay_us(200);
  MODIFY_REG(hwp_hpsys_rcc->CFGR, HPSYS_RCC_CFGR_TICKDIV_Msk,
             MAKE_REG_VAL(25, HPSYS_RCC_CFGR_TICKDIV_Msk, HPSYS_RCC_CFGR_TICKDIV_Pos));
  HAL_SYSTICK_Config(1920000 / RTC_TICKS_HZ);
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK_DIV8);

  SysTick->CTRL |= (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);

  // configure clock dividers for deep WFI:
  // HCLK = 48MHz / 12 = 4MHz
  // PCLK1 = 4MHz / 2^0 = 4MHz
  // PCLK2 = 4MHz / 2^1 = 2MHz
  HAL_RCC_HCPU_SetDeepWFIDiv(12, 0, 1);

  // TODO(SF32LB52): to use deep WFI when audio is ON, HCLK needs to remain
  // at 48MHz (div=1). Also, clock needs to be forced ON during deep WFI
  // (FORCE_HP bit in HPSYS_RCC_DBGR register)

  return true;
}

void AON_IRQHandler(void)
{
    uint32_t status;

    NVIC_DisableIRQ(AON_IRQn);
    HAL_HPAON_CLEAR_POWER_MODE();

    status = HAL_HPAON_GET_WSR();
    status &= ~HPSYS_AON_WSR_PIN_ALL;
    HAL_HPAON_CLEAR_WSR(status);
}

void SysTick_Handler(void) {
  extern void xPortSysTickHandler(void);
  xPortSysTickHandler();

  HAL_IncTick();

  prv_wdt_feed(1U);

  if (s_last_sleep_type == SleepTypeWfi) {
    s_analytics_wfi_ticks++;
    s_total_wfi_ticks++;
  } else if (s_last_sleep_type == SleepTypeDeepWfi) {
    s_analytics_deepwfi_ticks++;
    s_total_deepwfi_ticks++;
  }

  s_last_sleep_type = SleepTypeNone;

  // TODO(SF32LB52): we may need to handle tick loss compensation when using
  // SysTick due to flash erase times (runs with IRQs disabled to not interfere
  // with XIP, and can easily span multiple ticks)
}

void dump_current_runtime_stats(void) {
  uint32_t wfi_ticks = s_analytics_wfi_ticks;
  uint32_t deepwfi_ticks = s_analytics_deepwfi_ticks;
  uint32_t deepsleep_ticks = s_analytics_deepsleep_ticks;

  RtcTicks now_ticks = rtc_get_ticks();
  uint32_t total_ticks = (uint32_t)(now_ticks - s_last_ticks);
  uint32_t running_ticks = total_ticks - wfi_ticks - deepwfi_ticks - deepsleep_ticks;

  char buf[160];
  snprintf(buf, sizeof(buf), "Run:       %"PRIu32" ticks (%"PRIu32" %%)",
           running_ticks, (running_ticks * 100) / total_ticks);
  prompt_send_response(buf);
  snprintf(buf, sizeof(buf), "WFI:       %"PRIu32" ticks (%"PRIu32" %%)",
           wfi_ticks, (wfi_ticks * 100) / total_ticks);
  prompt_send_response(buf);
  snprintf(buf, sizeof(buf), "Deep WFI:  %"PRIu32" ticks (%"PRIu32" %%)",
           deepwfi_ticks, (deepwfi_ticks * 100) / total_ticks);
  prompt_send_response(buf);
  snprintf(buf, sizeof(buf), "Deepsleep: %"PRIu32" ticks (%"PRIu32" %%)",
           deepsleep_ticks, (deepsleep_ticks * 100) / total_ticks);
  prompt_send_response(buf);
  snprintf(buf, sizeof(buf), "Tot:       %"PRIu32" ticks", total_ticks);
  prompt_send_response(buf);
}

void command_force_wfi(const char *arg) {
  if (arg[0] == '1') {
    s_force_wfi = true;
    prompt_send_response("WFI forced ON (deep WFI and deep sleep disabled)");
  } else {
    s_force_wfi = false;
    prompt_send_response("WFI forced OFF (deep WFI and deep sleep allowed)");
  }
}

void pbl_analytics_external_collect_cpu_stats(void) {
  uint32_t wfi_ticks = s_analytics_wfi_ticks;
  uint32_t deepwfi_ticks = s_analytics_deepwfi_ticks;
  uint32_t deepsleep_ticks = s_analytics_deepsleep_ticks;

  RtcTicks now_ticks = rtc_get_ticks();
  uint32_t total_ticks = (uint32_t)(now_ticks - s_last_ticks);
  uint32_t running_ticks = total_ticks - wfi_ticks - deepwfi_ticks - deepsleep_ticks;

  // Calculate percentages
  uint16_t running_pct = 0;
  uint16_t wfi_pct = 0;
  uint16_t deepwfi_pct = 0;
  uint16_t deepsleep_pct = 0;

  if (total_ticks > 0) {
    running_pct = (uint16_t)((running_ticks * 10000ULL) / total_ticks);
    wfi_pct = (uint16_t)((wfi_ticks * 10000ULL) / total_ticks);
    deepwfi_pct = (uint16_t)((deepwfi_ticks * 10000ULL) / total_ticks);
    deepsleep_pct = (uint16_t)((deepsleep_ticks * 10000ULL) / total_ticks);
  }

  // SF32LB52: sleep0 = WFI, sleep1 = Deep WFI, sleep2 = Deep sleep
  PBL_ANALYTICS_SET_UNSIGNED(cpu_running_pct, running_pct);
  PBL_ANALYTICS_SET_UNSIGNED(cpu_sleep0_pct, wfi_pct);
  PBL_ANALYTICS_SET_UNSIGNED(cpu_sleep1_pct, deepwfi_pct);
  PBL_ANALYTICS_SET_UNSIGNED(cpu_sleep2_pct, deepsleep_pct);
  PBL_ANALYTICS_SET_UNSIGNED(sifli_ipc_not_idle_count, s_analytics_ipc_not_idle_count);

  s_last_ticks = now_ticks;
  s_analytics_wfi_ticks = 0;
  s_analytics_deepwfi_ticks = 0;
  s_analytics_deepsleep_ticks = 0;
  s_analytics_ipc_not_idle_count = 0;
}

// ---------------------------------------------------------------------------
// Rolling idle-state breakdown for the on-watch overlay.
//
// The chip's realistic resting floor is DEEP-WFI, not DEEPSLEEP: deep sleep
// needs a >=50 ms uninterrupted idle window that this workload rarely leaves,
// so the CPU keeps landing in deep WFI instead. We keep a small ring of
// (wall, deep, wfi) tick snapshots and, on read, compare the newest against the
// newest snapshot at least a minute old. That yields, over the trailing minute,
// the average milliseconds per wall-clock second the CPU spent deep (deep WFI
// or deeper), in light WFI, and actually running. Snapshots are taken lazily on
// read (the overlay reads once per wake), so this adds no wake-ups of its own;
// when the overlay is off, nothing samples and the ring costs nothing.
// ---------------------------------------------------------------------------

#define RATE_RING (16)
#define RATE_MIN_SPACING_TICKS (RTC_TICKS_HZ * 4)  // >=4s between kept samples -> >=60s span
#define RATE_WINDOW_TICKS (RTC_TICKS_HZ * 60)

static RtcTicks s_rate_wall[RATE_RING];
static RtcTicks s_rate_dsleep[RATE_RING];  // true deep sleep
static RtcTicks s_rate_dwfi[RATE_RING];    // deep WFI
static RtcTicks s_rate_wfi[RATE_RING];     // light WFI
static uint8_t s_rate_head;   // index of the newest sample
static uint8_t s_rate_count;

static void prv_rate_store(uint8_t idx, RtcTicks wall, RtcTicks dsleep, RtcTicks dwfi,
                           RtcTicks wfi) {
  s_rate_wall[idx] = wall;
  s_rate_dsleep[idx] = dsleep;
  s_rate_dwfi[idx] = dwfi;
  s_rate_wfi[idx] = wfi;
}

static void prv_rate_sample(void) {
  __disable_irq();
  const RtcTicks wall = rtc_get_ticks();
  const RtcTicks dsleep = s_total_deepsleep_ticks;
  const RtcTicks dwfi = s_total_deepwfi_ticks;
  const RtcTicks wfi = s_total_wfi_ticks;
  __enable_irq();

  if (s_rate_count == 0U) {
    s_rate_head = 0U;
    prv_rate_store(0U, wall, dsleep, dwfi, wfi);
    s_rate_count = 1U;
    return;
  }
  if ((wall - s_rate_wall[s_rate_head]) < RATE_MIN_SPACING_TICKS) {
    // Too soon since the last kept sample: fold into it so a burst of reads
    // can't evict the minute of history the average needs.
    prv_rate_store(s_rate_head, wall, dsleep, dwfi, wfi);
    return;
  }
  s_rate_head = (uint8_t)((s_rate_head + 1U) % RATE_RING);
  prv_rate_store(s_rate_head, wall, dsleep, dwfi, wfi);
  if (s_rate_count < RATE_RING) {
    s_rate_count++;
  }
}

void soc_sf32lb_cpu_stats_init(void) {
  s_rate_head = 0U;
  s_rate_count = 0U;
}

void soc_sf32lb_idle_ms_per_s(uint16_t *dsleep_out, uint16_t *deepwfi_out, uint16_t *wfi_out,
                              uint16_t *run_out) {
  uint16_t dsleep_ms = 0U;
  uint16_t dwfi_ms = 0U;
  uint16_t wfi_ms = 0U;

  prv_rate_sample();
  if (s_rate_count >= 2U) {
    const RtcTicks wall_now = s_rate_wall[s_rate_head];
    // Default anchor is the oldest sample; prefer the newest one that is at
    // least a minute old so the average settles to a true trailing-60s window.
    uint8_t anchor = (uint8_t)((s_rate_head + RATE_RING - (s_rate_count - 1U)) % RATE_RING);
    for (uint8_t i = 1U; i < s_rate_count; i++) {
      const uint8_t idx = (uint8_t)((s_rate_head + RATE_RING - i) % RATE_RING);
      if ((wall_now - s_rate_wall[idx]) >= RATE_WINDOW_TICKS) {
        anchor = idx;
        break;
      }
    }
    const RtcTicks wall_span = wall_now - s_rate_wall[anchor];
    if (wall_span != 0U) {
      dsleep_ms = (uint16_t)(((s_rate_dsleep[s_rate_head] - s_rate_dsleep[anchor]) * 1000ULL) /
                             wall_span);
      dwfi_ms = (uint16_t)(((s_rate_dwfi[s_rate_head] - s_rate_dwfi[anchor]) * 1000ULL) /
                           wall_span);
      wfi_ms = (uint16_t)(((s_rate_wfi[s_rate_head] - s_rate_wfi[anchor]) * 1000ULL) / wall_span);
      if (dsleep_ms > 1000U) {
        dsleep_ms = 1000U;
      }
      if (dwfi_ms > (uint16_t)(1000U - dsleep_ms)) {
        dwfi_ms = (uint16_t)(1000U - dsleep_ms);
      }
      if (wfi_ms > (uint16_t)(1000U - dsleep_ms - dwfi_ms)) {
        wfi_ms = (uint16_t)(1000U - dsleep_ms - dwfi_ms);
      }
    }
  }

  if (dsleep_out) {
    *dsleep_out = dsleep_ms;
  }
  if (deepwfi_out) {
    *deepwfi_out = dwfi_ms;
  }
  if (wfi_out) {
    *wfi_out = wfi_ms;
  }
  if (run_out) {
    *run_out = (uint16_t)(1000U - dsleep_ms - dwfi_ms - wfi_ms);
  }
}

void soc_sf32lb_cpu_time_get(SocSf32lbCpuTime *out) {
  __disable_irq();
  out->wall_ticks = rtc_get_ticks();
  out->wfi_ticks = s_total_wfi_ticks;
  out->deepwfi_ticks = s_total_deepwfi_ticks;
  out->deepsleep_ticks = s_total_deepsleep_ticks;
  __enable_irq();
}
