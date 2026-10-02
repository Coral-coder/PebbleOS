/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "comm/ble/multi_phone.h"
#include "pbl/util/attributes.h"

//! Single-phone behavior: tests exercise the upstream-equivalent paths unless
//! they override these.
uint8_t WEAK multi_phone_max_connections(void) {
  return 1;
}

bool WEAK multi_phone_should_fill_another_slot(void) {
  return false;
}

void WEAK multi_phone_mode_changed(void) {
}
