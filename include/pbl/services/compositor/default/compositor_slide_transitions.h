/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/compositor/compositor.h"

const CompositorTransition *compositor_slide_transition_timeline_get(bool timeline_is_future,
                                                                     bool timeline_is_destination,
                                                                     bool timeline_is_empty);

//! Full-screen vertical push into an app: the incoming app framebuffer slides in, pushing
//! the current framebuffer out. With slide_up the app enters from the bottom.
const CompositorTransition *compositor_slide_transition_app_get(bool slide_up);
