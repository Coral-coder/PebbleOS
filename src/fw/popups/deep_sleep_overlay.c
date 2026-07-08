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
#include "pbl/soc/sf32lb/sleep.h"

#include <stdio.h>

static Window s_window;
static bool s_on_screen;

static void prv_update_proc(Layer *layer, GContext *ctx) {
  const uint16_t deep = soc_sf32lb_deep_sleep_ms_per_s();
  const uint16_t awake = (deep >= 1000U) ? 0U : (uint16_t)(1000U - deep);

  char sleep_line[24];
  char awake_line[24];
  snprintf(sleep_line, sizeof(sleep_line), "%u ms/s asleep", deep);
  snprintf(awake_line, sizeof(awake_line), "%u ms/s awake", awake);

  const GRect bounds = layer->bounds;
  const GRect box = GRect(4, STATUS_BAR_LAYER_HEIGHT + 2, bounds.size.w - 8, 44);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_round_rect(ctx, &box, 4, GCornersAll);

  graphics_context_set_text_color(ctx, GColorWhite);
  const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  const GRect line1 = GRect(box.origin.x + 4, box.origin.y + 1, box.size.w - 8, 21);
  const GRect line2 = GRect(box.origin.x + 4, box.origin.y + 22, box.size.w - 8, 21);
  graphics_draw_text(ctx, sleep_line, font, line1, GTextOverflowModeFill, GTextAlignmentCenter,
                     NULL);
  graphics_draw_text(ctx, awake_line, font, line2, GTextOverflowModeFill, GTextAlignmentCenter,
                     NULL);
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
