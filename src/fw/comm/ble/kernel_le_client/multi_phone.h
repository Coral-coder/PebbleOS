/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! Maximum number of phones that can be simultaneously connected.
#define MAX_PHONE_CONNECTIONS 2

//! Identifies which phone slot (0 or 1) a connection occupies.
typedef uint8_t PhoneSlot;

#define PHONE_SLOT_INVALID 0xFF

//! Runtime connection limit: MAX_PHONE_CONNECTIONS in dual-phone mode, 1 in
//! single-phone mode (Settings -> System -> Debugging -> Dual Phone BT).
//! Slot storage stays sized at MAX_PHONE_CONNECTIONS either way; the limit
//! only gates whether advertising resumes to fill another slot.
uint8_t multi_phone_max_connections(void);

//! Re-evaluate advertising after the dual-phone preference changed. Safe to
//! call from any task; the work is marshaled to KernelMain.
void multi_phone_mode_changed(void);
