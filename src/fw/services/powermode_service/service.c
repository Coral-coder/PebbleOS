/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/powermode_service.h"

#include "drivers/cpumode.h"
#include "drivers/rtc.h"
#include "os/mutex.h"
#include "pbl/services/new_timer/new_timer.h"
#include "system/logging.h"
#include "system/passert.h"

#include <stdint.h>

PBL_LOG_MODULE_DEFINE(service_powermode_service, CONFIG_SERVICE_POWERMODE_SERVICE_LOG_LEVEL);

// Tail delay before downclocking after the last HP demand ends.
#define DOWNSCALE_DELAY_MS 100
// Periodic check that CPU mode matches idle demand.
#define IDLE_POLL_MS 500
// Minimum extension per boost call.
#define BOOST_MIN_MS 50

static uint32_t s_refcount;
static PebbleMutex *s_mutex;
static bool s_enabled;
static bool s_boot_complete;
static bool s_at_low_power;
static RtcTicks s_boost_until_ticks;
static TimerID s_downscale_timer = TIMER_INVALID_ID;
static TimerID s_idle_timer = TIMER_INVALID_ID;

static void prv_downscale_timer_cb(void *data);

static bool prv_boost_active(void) {
  return rtc_get_ticks() < s_boost_until_ticks;
}

static bool prv_wants_high_performance(void) {
  if (!s_enabled || !s_boot_complete) {
    return true;
  }
  return s_refcount > 0 || prv_boost_active();
}

static void prv_enter_high_performance(void) {
  if (s_at_low_power) {
    cpumode_set(CPUMode_HighPerformance);
    s_at_low_power = false;
    PBL_LOG_DBG("CPU HP");
  }
  new_timer_stop(s_downscale_timer);
}

static void prv_schedule_downscale(void) {
  new_timer_start(s_downscale_timer, DOWNSCALE_DELAY_MS, prv_downscale_timer_cb, NULL, 0);
}

static void prv_enter_low_power(void) {
  if (!s_at_low_power) {
    cpumode_set(CPUMode_LowPower);
    s_at_low_power = true;
    PBL_LOG_DBG("CPU LP");
  }
}

static void prv_apply_demand(void) {
  if (!s_enabled) {
    prv_enter_high_performance();
    return;
  }

  if (!s_boot_complete) {
    prv_enter_high_performance();
    return;
  }

  if (prv_wants_high_performance()) {
    prv_enter_high_performance();
  } else {
    prv_schedule_downscale();
  }
}

static void prv_downscale_timer_cb(void *data) {
  (void)data;

  mutex_lock(s_mutex);

  if (!s_enabled || !s_boot_complete) {
    mutex_unlock(s_mutex);
    return;
  }

  if (!prv_wants_high_performance()) {
    prv_enter_low_power();
  }

  mutex_unlock(s_mutex);
}

static void prv_idle_timer_cb(void *data) {
  (void)data;

  mutex_lock(s_mutex);
  prv_apply_demand();
  mutex_unlock(s_mutex);
}

void powermode_service_init(void) {
  s_refcount = 0;
  s_mutex = mutex_create();
  s_boost_until_ticks = 0;
  s_at_low_power = false;

  s_downscale_timer = new_timer_create();
  s_idle_timer = new_timer_create();
  new_timer_start(s_idle_timer, IDLE_POLL_MS, prv_idle_timer_cb, NULL,
                  TIMER_START_FLAG_REPEATING);
}

void powermode_service_boot_complete(void) {
  mutex_lock(s_mutex);
  s_boot_complete = true;
  prv_apply_demand();
  mutex_unlock(s_mutex);
}

void powermode_service_set_enabled(bool enabled) {
  mutex_lock(s_mutex);
  s_enabled = enabled;
  prv_apply_demand();
  mutex_unlock(s_mutex);
}

void powermode_service_boost_ms(uint32_t ms) {
  if (!s_enabled) {
    return;
  }

  if (ms < BOOST_MIN_MS) {
    ms = BOOST_MIN_MS;
  }

  mutex_lock(s_mutex);

  const RtcTicks now = rtc_get_ticks();
  const RtcTicks extend = ((uint64_t)ms * RTC_TICKS_HZ) / 1000;
  const RtcTicks until = now + extend;
  if (until > s_boost_until_ticks) {
    s_boost_until_ticks = until;
  }

  prv_apply_demand();
  mutex_unlock(s_mutex);
}

void powermode_service_request_hp(void) {
  if (!s_enabled) {
    return;
  }

  mutex_lock(s_mutex);

  if (s_refcount == 0) {
    prv_enter_high_performance();
  }

  s_refcount++;

  mutex_unlock(s_mutex);
}

void powermode_service_release_hp(void) {
  if (!s_enabled) {
    return;
  }

  mutex_lock(s_mutex);

  if (s_refcount == 0) {
    mutex_unlock(s_mutex);
    return;
  }

  s_refcount--;

  if (s_refcount == 0) {
    prv_apply_demand();
  }

  mutex_unlock(s_mutex);
}
