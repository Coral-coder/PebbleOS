/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/timeline/reminders.h"

#include <pbl/drivers/rtc.h>
#include "kernel/event_loop.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "pbl/services/regular_timer.h"
#include "pbl/services/system_task.h"
#include "pbl/services/blob_db/pin_db.h"
#include "pbl/services/blob_db/reminder_db.h"
#include "pbl/services/timeline/item.h"
#include "pbl/util/math.h"
#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DECLARE(service_timeline, CONFIG_SERVICE_TIMELINE_LOG_LEVEL);

#define INVALID_SNOOZE_DELAY 0
#define HALF_SNOOZE_END_MARK 30 // Seconds
#define CONSTANT_SNOOZE_DELAY (10 * SECONDS_PER_MINUTE) // Seconds
#define CONSTANT_SNOOZE_END_MARK (48 * MINUTES_PER_HOUR * SECONDS_PER_MINUTE) // Seconds

// Poll every second only once within this many seconds of the deadline, so the
// reminder still fires within ~1 s of its timestamp (as the legacy 1 Hz poll
// did). Further out, poll coarsely so the CPU is not woken every second.
#define REMINDER_FINE_WINDOW_S 5
// Cap on the coarse poll period. Bounds how late a reminder can fire after an
// un-notified forward clock step between two polls; significant steps arrive
// via PEBBLE_SET_TIME_EVENT and re-tighten the poll immediately regardless.
#define REMINDER_MAX_POLL_S 300

static RegularTimerInfo s_reminder_timer;
static bool s_reminder_armed;
static time_t s_next_reminder_timestamp;
static ReminderId s_next_reminder_id;

bool reminders_mark_has_reminded(ReminderId *reminder_id);

static void prv_put_reminder_event(ReminderId *reminder_id, ReminderEventType type) {
  Uuid *removed_id = kernel_malloc(sizeof(Uuid));
  if (!removed_id) {
    return;
  }

  *removed_id = *reminder_id;
  PebbleEvent event = {
    .type = PEBBLE_REMINDER_EVENT,
    .reminder = {
      .type = type,
      .reminder_id = removed_id,
    }
  };
  event_put(&event);
}

void reminders_handle_reminder_updated(const Uuid *reminder_id) {
  prv_put_reminder_event((ReminderId *)reminder_id, ReminderUpdated);
}

void reminders_handle_reminder_removed(const Uuid *reminder_id) {
  prv_put_reminder_event((ReminderId *)reminder_id, ReminderRemoved);
}

static void prv_trigger_reminder_system_task_callback(void *data) {
  ReminderId *item_id = (ReminderId *)data;

  // Mark that we are about to display the reminder
  if (!reminders_mark_has_reminded(item_id)) {
    return;
  }

  prv_put_reminder_event(item_id, ReminderTriggered);
  reminders_update_timer();
}

// Poll period for the current deadline: 1 s inside the final window (so the
// reminder still fires within ~1 s of its timestamp), coarser further out.
static uint16_t prv_poll_period(time_t now) {
  int32_t delta = (int32_t)(s_next_reminder_timestamp - now);
  if (delta <= REMINDER_FINE_WINDOW_S) {
    return 1;
  }
  // Wake no later than the fine-window boundary, then re-tighten to 1 s.
  uint32_t coarse = (uint32_t)(delta - REMINDER_FINE_WINDOW_S);
  return (uint16_t)MIN(coarse, (uint32_t)REMINDER_MAX_POLL_S);
}

// (Re-)arm the RTC poll for the armed reminder, or deregister it entirely when
// nothing is pending so the CPU is never woken on the reminder's account.
static void prv_reschedule_poll(void) {
  if (!s_reminder_armed) {
    if (regular_timer_is_scheduled(&s_reminder_timer)) {
      regular_timer_remove_callback(&s_reminder_timer);
    }
    return;
  }
  // cb is installed once in reminders_init(); add_multisecond only (re)arms it.
  regular_timer_add_multisecond_callback(&s_reminder_timer, prv_poll_period(rtc_get_time()));
}

// Fires against the RTC. Rather than a fixed 1 Hz poll, the period is widened
// while the deadline is far out and tightened to 1 s as it approaches, so an
// idle watch with a distant reminder is not woken every second.
static void prv_timer_callback(void *data) {
  if (!s_reminder_armed) {
    return;
  }
  time_t now = rtc_get_time();
  if (s_next_reminder_timestamp > now) {
    // Not due yet: re-tighten the poll as the deadline approaches.
    regular_timer_add_multisecond_callback(&s_reminder_timer, prv_poll_period(now));
    return;
  }
  if (system_task_add_callback(prv_trigger_reminder_system_task_callback,
                               &s_next_reminder_id)) {
    s_reminder_armed = false;
  }
}

static status_t prv_set_timer(Reminder *item) {
  s_next_reminder_id = item->header.id;
  s_next_reminder_timestamp = item->header.timestamp;
  s_reminder_armed = true;
  PBL_LOG_DBG("Set reminder for %ld", s_next_reminder_timestamp);
  return S_SUCCESS;
}

status_t reminders_update_timer(void) {
  PBL_LOG_DBG("Attempting to update timer.");
  s_reminder_armed = false;

  TimelineItem item = {{{0}}};
  status_t rv = reminder_db_next_item_header(&item);
  if (rv == S_NO_MORE_ITEMS) {
    PBL_LOG_DBG("No more reminders to add to queue.");
    prv_reschedule_poll();
    return S_SUCCESS;
  } else if (rv) {
    return rv;
  }

  rv = prv_set_timer(&item);
  prv_reschedule_poll();
  return rv;
}

void reminders_handle_clock_change(void) {
  // The poll may be sitting on a coarse period; a clock step can move the
  // deadline in or out from under it, so re-evaluate against the new time.
  prv_reschedule_poll();
}

status_t reminders_insert(Reminder *reminder) {
  status_t rv = reminder_db_insert_item(reminder);
  return rv;
}

status_t reminders_init(void) {
  // The poll is armed on demand by reminders_update_timer() and removed again
  // when no reminder is pending, so there is no standing 1 Hz callback here.
  s_reminder_timer.cb = prv_timer_callback;
  return reminders_update_timer();
}

status_t reminders_delete(ReminderId *reminder_id) {
  return reminder_db_delete_item(reminder_id, true /* send_event */);
}

T_STATIC uint32_t prv_calculate_snooze_delay(TimelineItem *item) {
  time_t current_time_utc = rtc_get_time();
  time_t reminder_time_utc = item->header.timestamp;
  if (current_time_utc <= reminder_time_utc) {
    return INVALID_SNOOZE_DELAY;
  }

  uint32_t snooze_delay;

  // Get parent pin
  const TimelineItemId *parent_id = &item->header.parent_id;
  TimelineItem parent_item;
  status_t status = pin_db_get(parent_id, &parent_item);
  if (status != S_SUCCESS) {
    return INVALID_SNOOZE_DELAY;
  }

  // Snooze logic:
  // If current_time is more than HALF_SNOOZE_END_MARK before event_time, snooze for half the
  //   remaining time until the event.
  // If current_time is less than HALF_SNOOZE_END_MARK before event_time, and not more than
  //   CONSTANT_SNOOZE_END_MARK after event_time, snooze for CONSTANT_SNOOZE_DELAY.
  // If current_time is more than CONSTANT_SNOOZE_END_MARK after event_time, don't snooze.
  time_t event_time_utc = parent_item.header.timestamp;
  if (event_time_utc > current_time_utc &&
      event_time_utc - current_time_utc > HALF_SNOOZE_END_MARK) {
    // Half-time snooze
    snooze_delay = (event_time_utc - reminder_time_utc) / 2;
  } else if (current_time_utc > event_time_utc &&
             current_time_utc - event_time_utc > CONSTANT_SNOOZE_END_MARK) {
    // Stop snoozing
    snooze_delay = INVALID_SNOOZE_DELAY;
  } else {
    // Constant-time snooze
    snooze_delay = CONSTANT_SNOOZE_DELAY;
  }

  timeline_item_free_allocated_buffer(&parent_item);
  return snooze_delay;
}

bool reminders_can_snooze(Reminder *reminder) {
  return (prv_calculate_snooze_delay((TimelineItem *)reminder) > 0);
}

status_t reminders_snooze(Reminder *reminder) {
  uint32_t snooze_delay = prv_calculate_snooze_delay((TimelineItem *)reminder);
  if (snooze_delay == 0) {
    return E_INVALID_OPERATION;
  }

  // Modify reminder timestamp
  TimelineItem *item = (TimelineItem*) reminder;
  item->header.timestamp = rtc_get_time() + (time_t) snooze_delay;

  // Unset the reminded status
  item->header.reminded = false;

  // Reinsert the reminder
  return reminders_insert(reminder);
}

// only used for tests
RegularTimerInfo *get_reminder_timer(void) {
  return &s_reminder_timer;
}

bool get_reminder_armed(void) {
  return s_reminder_armed;
}

time_t get_reminder_timestamp(void) {
  return s_next_reminder_timestamp;
}

ReminderId *get_reminder_id(void) {
  return &s_next_reminder_id;
}

bool reminders_mark_has_reminded(ReminderId *reminder_id) {
  status_t rv = reminder_db_set_status_bits(reminder_id, TimelineItemStatusReminded);
  return (rv == S_SUCCESS);
}
