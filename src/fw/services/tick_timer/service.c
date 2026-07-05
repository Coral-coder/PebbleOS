/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/tick_timer.h"

#include "kernel/events.h"
#include "drivers/rtc.h"
#include "pbl/services/regular_timer.h"
#include "process_management/app_manager.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/time/time.h"

PBL_LOG_MODULE_DEFINE(service_tick_timer, CONFIG_SERVICE_TICK_TIMER_LOG_LEVEL);

static uint16_t s_num_subscribers;

// Per-task flag for whether that task needs second-resolution ticks, plus the
// running count of tasks that do. The publisher is registered twice: a minutes
// node that is always on while anyone subscribes (fires exactly at minute
// boundaries), and a seconds node that is only registered while some task
// needs second-resolution ticks. Two separate nodes avoid migrating a single
// callback between regular_timer lists, which would race against a concurrent
// execution of the callback. When both fire on the same tick (a minute
// boundary in seconds mode) the publisher dedupes on s_last_published.
static bool s_task_needs_seconds[NumPebbleTask];
static uint16_t s_seconds_subscribers;

//! Epoch seconds of the most recently broadcast tick, used both to detect
//! minute rollover and to dedupe the minute-boundary tick when the seconds
//! and minutes nodes fire back-to-back.
static time_t s_last_published;

static void prv_publish_tick(time_t now) {
  s_last_published = now;
  PebbleEvent e = {
    .type = PEBBLE_TICK_EVENT,
    .clock_tick.tick_time = now,
  };
  event_put(&e);
}

static void timer_tick_event_publisher(void* data) {
  const time_t now = rtc_get_time();
  if (now == s_last_published) {
    // Already published a tick for this very second (e.g. the seconds and
    // minutes nodes both fired on a minute boundary).
    return;
  }
  if (s_seconds_subscribers == 0 &&
      (now / SECONDS_PER_MINUTE) == (s_last_published / SECONDS_PER_MINUTE)) {
    // No second-resolution subscribers and still within the same minute as the
    // last tick: nothing for minute-granularity subscribers to do.
    return;
  }
  prv_publish_tick(now);
}

static RegularTimerInfo s_tick_timer_minutes_info = {
  .cb = &timer_tick_event_publisher
};

static RegularTimerInfo s_tick_timer_seconds_info = {
  .cb = &timer_tick_event_publisher
};

static void prv_update_seconds_registration(void) {
  const bool want_seconds = (s_num_subscribers > 0) && (s_seconds_subscribers > 0);
  const bool registered = regular_timer_is_scheduled(&s_tick_timer_seconds_info) &&
                          !regular_timer_pending_deletion(&s_tick_timer_seconds_info);
  if (want_seconds && !registered) {
    regular_timer_add_seconds_callback(&s_tick_timer_seconds_info);
  } else if (!want_seconds && registered) {
    regular_timer_remove_callback(&s_tick_timer_seconds_info);
  }
}

void tick_timer_add_subscriber(PebbleTask task) {
  ++s_num_subscribers;
  if (s_num_subscribers == 1) {
    PBL_LOG_DBG("starting tick timer");
    regular_timer_add_minutes_callback(&s_tick_timer_minutes_info);
    prv_update_seconds_registration();
  }
  // Give the new subscriber a prompt baseline tick instead of making it wait up
  // to a full minute for the next boundary.
  prv_publish_tick(rtc_get_time());
}

void tick_timer_remove_subscriber(PebbleTask task) {
  PBL_ASSERTN(s_num_subscribers > 0);
  // Drop any second-resolution request this task still held.
  tick_timer_set_seconds_subscribed(task, false);
  --s_num_subscribers;
  if (s_num_subscribers == 0) {
    PBL_LOG_DBG("stopping tick timer");
    regular_timer_remove_callback(&s_tick_timer_minutes_info);
    prv_update_seconds_registration();
  }
}

void tick_timer_handle_clock_change(void) {
  if (s_num_subscribers == 0) {
    return;
  }
  const time_t now = rtc_get_time();
  if (now != s_last_published) {
    prv_publish_tick(now);
  }
}

void tick_timer_set_seconds_subscribed(PebbleTask task, bool needs_seconds) {
  PBL_ASSERTN(task < NumPebbleTask);
  if (s_task_needs_seconds[task] == needs_seconds) {
    return;
  }
  s_task_needs_seconds[task] = needs_seconds;
  if (needs_seconds) {
    ++s_seconds_subscribers;
  } else {
    PBL_ASSERTN(s_seconds_subscribers > 0);
    --s_seconds_subscribers;
  }
  prv_update_seconds_registration();
}
