/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! Sleep levels, ordered from shallowest to deepest.
typedef enum {
  SOC_SF32LB_ACTIVE = 0,  //!< No sleep at all
  SOC_SF32LB_WFI,         //!< Light WFI
  SOC_SF32LB_DEEPWFI,     //!< Deep WFI
  SOC_SF32LB_DEEPSLEEP,   //!< Deep sleep
} SocSf32lbSleepLevel;

//! Block the given sleep level and every deeper level. For example,
//! soc_sf32lb_sleep_block(SOC_SF32LB_DEEPWFI) forbids deep WFI and deep sleep,
//! leaving plain WFI as the deepest permitted level. SOC_SF32LB_ACTIVE cannot
//! be blocked. Refcounted; balance each call with soc_sf32lb_sleep_release().
//! Safe to call concurrently. With no blocks, the deepest permitted level is
//! SOC_SF32LB_DEEPSLEEP.
void soc_sf32lb_sleep_block(SocSf32lbSleepLevel level);

//! Release a block taken with soc_sf32lb_sleep_block(level).
void soc_sf32lb_sleep_release(SocSf32lbSleepLevel level);

//! Deepest sleep level currently permitted (one step shallower than the
//! shallowest outstanding block).
SocSf32lbSleepLevel soc_sf32lb_sleep_max_level(void);

//! Cumulative (since-boot) time spent in each CPU sleep state, plus the
//! monotonic wall-clock, all in RTC ticks (RTC_TICKS_HZ). For the on-watch
//! Deep Sleep Stats screen.
typedef struct {
  uint64_t wall_ticks;
  uint64_t wfi_ticks;
  uint64_t deepwfi_ticks;
  uint64_t deepsleep_ticks;
} SocSf32lbCpuTime;

void soc_sf32lb_cpu_time_get(SocSf32lbCpuTime *out);

//! Reset the rolling deep-sleep rate history. Call once at boot.
void soc_sf32lb_cpu_stats_init(void);

//! Breakdown of how the CPU spent the trailing minute, in average milliseconds
//! per wall-clock second (each 0-1000, summing to ~1000):
//!   dsleep  - true deep sleep (needs a >=50 ms uninterrupted idle window)
//!   deepwfi - deep WFI
//!   wfi     - light WFI
//!   run     - actually executing
//! Any out pointer may be NULL. Sampled lazily on each call, so callers drive
//! the resolution.
void soc_sf32lb_idle_ms_per_s(uint16_t *dsleep_out, uint16_t *deepwfi_out, uint16_t *wfi_out,
                              uint16_t *run_out);
