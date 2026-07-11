/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "deep_sleep_overlay.h"

#include "shell/prefs.h"

#if defined(CONFIG_SOC_SF32LB52)

#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/text.h"
#include "applib/ui/status_bar_layer.h"
#include "applib/ui/window.h"
#include "applib/ui/window_stack.h"
#include "kernel/event_loop.h"
#include "kernel/ui/modals/modal_manager.h"
#include "pbl/services/battery/battery_monitor.h"
#include "pbl/services/battery/battery_state.h"
#include "pbl/soc/sf32lb/sleep.h"

#include <inttypes.h>
#include <stdio.h>

static Window s_window;
static bool s_on_screen;

//! Full-width sliver pinned to the very top of the screen: ONE row, always.
//! Idle states on the left, projected battery life right-aligned. If the
//! labeled forms would collide, both drop to compact forms that always fit;
//! the strip never grows a second row.
#define HUD_LINE_HEIGHT (16)
#define HUD_TEXT_GAP (10)

static void prv_update_proc(Layer *layer, GContext *ctx) {
  uint16_t dsleep = 0U;
  uint16_t dwfi = 0U;
  uint16_t wfi = 0U;
  uint16_t run = 0U;
  soc_sf32lb_idle_ms_per_s(&dsleep, &dwfi, &wfi, &run);

  const BatteryChargeState charge_state = battery_get_charge_state();
  const uint32_t tte_s = battery_state_get_time_to_empty_s();
  const uint32_t tte_d = tte_s / (24U * 60U * 60U);
  const uint32_t tte_h = (tte_s % (24U * 60U * 60U)) / (60U * 60U);
  const uint32_t tte_m = (tte_s % (60U * 60U)) / 60U;

  char idle_line[48];
  snprintf(idle_line, sizeof(idle_line), "ds %u  dw %u  w %u  r %u", dsleep, dwfi, wfi, run);

  char batt_line[48];
  if (charge_state.is_charging) {
    snprintf(batt_line, sizeof(batt_line), "charging");
  } else if (tte_s == 0U) {
    snprintf(batt_line, sizeof(batt_line), "est...");
  } else {
    snprintf(batt_line, sizeof(batt_line), "%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m", tte_d, tte_h,
             tte_m);
  }

  const GRect bounds = layer->bounds;
  const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int16_t x = 4;
  const int16_t w = bounds.size.w - 8;
  const GRect measure_box = GRect(0, 0, bounds.size.w, HUD_LINE_HEIGHT + 2);

  const GSize idle_size = graphics_text_layout_get_max_used_size(
      ctx, idle_line, font, measure_box, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  const GSize batt_size = graphics_text_layout_get_max_used_size(
      ctx, batt_line, font, measure_box, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  if ((idle_size.w + HUD_TEXT_GAP + batt_size.w) > w) {
    // Compact forms; still one row, never two.
    snprintf(idle_line, sizeof(idle_line), "%u/%u/%u/%u", dsleep, dwfi, wfi, run);
    if (charge_state.is_charging) {
      snprintf(batt_line, sizeof(batt_line), "chg");
    } else if (tte_s == 0U) {
      snprintf(batt_line, sizeof(batt_line), "est");
    } else {
      snprintf(batt_line, sizeof(batt_line), "%" PRIu32 "d%" PRIu32 "h%" PRIu32 "m", tte_d, tte_h,
               tte_m);
    }
  }

  const GRect box = GRect(0, 0, bounds.size.w, HUD_LINE_HEIGHT + 2);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, &box);

  graphics_context_set_text_color(ctx, GColorWhite);
  // Gothic 14 renders with ~2 px of leading; shift up so the row fits the sliver.
  const GRect line = GRect(x, -2, w, HUD_LINE_HEIGHT + 2);
  graphics_draw_text(ctx, idle_line, font, line, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, batt_line, font, line, GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

static void prv_push_cb(void *unused) {
  if (s_on_screen) {
    return;
  }
  s_on_screen = true;
  window_init(&s_window, WINDOW_NAME("Deep Sleep HUD"));
  window_set_focusable(&s_window, false);
  window_set_transparent(&s_window, true);
  layer_set_update_proc(&s_window.layer, prv_update_proc);
  modal_window_push(&s_window, ModalPriorityDiscreet, false);
}

static void prv_pop_cb(void *unused) {
  if (!s_on_screen) {
    return;
  }
  s_on_screen = false;
  window_stack_remove(&s_window, false);
}

void deep_sleep_overlay_init(void) {
  if (shell_prefs_get_deep_sleep_overlay_enabled()) {
    launcher_task_add_callback(prv_push_cb, NULL);
  }
}

void deep_sleep_overlay_set_enabled(bool enabled) {
  shell_prefs_set_deep_sleep_overlay_enabled(enabled);
  launcher_task_add_callback(enabled ? prv_push_cb : prv_pop_cb, NULL);
}

bool deep_sleep_overlay_is_enabled(void) {
  return shell_prefs_get_deep_sleep_overlay_enabled();
}

#else  // !CONFIG_SOC_SF32LB52

void deep_sleep_overlay_init(void) {}
void deep_sleep_overlay_set_enabled(bool enabled) {}
bool deep_sleep_overlay_is_enabled(void) { return false; }

#endif  // CONFIG_SOC_SF32LB52
