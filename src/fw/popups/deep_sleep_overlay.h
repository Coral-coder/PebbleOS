/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

//! Debug HUD that floats over the watchface showing how many milliseconds per
//! second the chip spent in (and out of) deep sleep, averaged over the last
//! minute. Redraws on the next wake, so it does not keep the watch awake.

//! Push the overlay if the persisted toggle is on. Call once at boot.
void deep_sleep_overlay_init(void);

//! Turn the overlay on or off. Persists the choice and shows/hides the HUD.
void deep_sleep_overlay_set_enabled(bool enabled);

//! Whether the overlay toggle is currently on.
bool deep_sleep_overlay_is_enabled(void);
