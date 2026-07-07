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

//! Start the per-minute deep-sleep residency sampler. Call once after the
//! regular-timer service is up.
void soc_sf32lb_cpu_stats_init(void);

//! Fraction of wall-clock time spent in deep sleep over the last `minutes`
//! minutes (capped at the available history), expressed in per-mille (0-1000).
uint16_t soc_sf32lb_deep_sleep_residency_permille(uint8_t minutes);
