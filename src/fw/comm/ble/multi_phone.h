/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Dual-phone policy. Unlike the first-generation implementation, no
//! per-phone "slot" identity is threaded through the BLE stack: connections
//! are keyed by GAPLEConnection everywhere, and this module only answers
//! policy questions (how many phones may connect, should we keep advertising
//! to let another bonded phone in).

//! Maximum number of phones that can be simultaneously connected. The NimBLE
//! host is provisioned for this many links (BLE_MAX_CONNECTIONS).
#define MAX_PHONE_CONNECTIONS 2

//! Runtime connection limit: MAX_PHONE_CONNECTIONS when the dual-phone
//! preference is enabled, 1 otherwise.
uint8_t multi_phone_max_connections(void);

//! True when reconnect advertising should keep running even though a phone is
//! already connected: dual-phone mode is on and fewer phones are connected
//! than are bonded (capped at the connection limit).
bool multi_phone_should_fill_another_slot(void);

//! Re-evaluate advertising after the dual-phone preference changed. Safe to
//! call from any task; the work is marshaled to KernelMain.
void multi_phone_mode_changed(void);
