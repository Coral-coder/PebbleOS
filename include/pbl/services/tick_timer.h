/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/pebble_tasks.h"

#include <stdbool.h>

void tick_timer_add_subscriber(PebbleTask task);
void tick_timer_remove_subscriber(PebbleTask task);

//! @internal
//! Report whether \a task needs second-resolution ticks. When no task needs
//! seconds, the tick event is only broadcast when the minute changes so that
//! minute-granularity subscribers don't wake the app/worker tasks every second.
void tick_timer_set_seconds_subscribed(PebbleTask task, bool needs_seconds);

//! @internal
//! Publish a fresh tick after the wall clock changed. In minute-granularity
//! mode the publisher only runs at minute boundaries, so without this a clock
//! change could leave subscribers showing a stale minute until the next
//! boundary.
void tick_timer_handle_clock_change(void);
