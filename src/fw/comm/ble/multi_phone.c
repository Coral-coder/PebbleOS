/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "multi_phone.h"

#include "comm/ble/gap_le_connect.h"
#include "comm/ble/gap_le_slave_reconnect.h"
#include "kernel/event_loop.h"
#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"
#include "shell/prefs.h"

uint8_t multi_phone_max_connections(void) {
  return shell_prefs_get_bt_dual_phone_enabled() ? MAX_PHONE_CONNECTIONS : 1;
}

bool multi_phone_should_fill_another_slot(void) {
  const uint8_t limit = multi_phone_max_connections();
  if (limit <= 1) {
    return false;
  }
  uint8_t bonded = bt_persistent_storage_get_ble_bonding_count();
  if (bonded > limit) {
    bonded = limit;
  }
  return gap_le_connect_slave_connection_count() < bonded;
}

static void prv_mode_changed_kernelmain_cb(void *unused) {
  // Re-evaluate reconnect advertising under the new limit: stop clears the
  // current job, start re-applies the (now different) fill policy.
  gap_le_slave_reconnect_stop();
  gap_le_slave_reconnect_start();
}

void multi_phone_mode_changed(void) {
  launcher_task_add_callback(prv_mode_changed_kernelmain_cb, NULL);
}
