/*
 * Stereo scanouts on a Wayland subsurface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef UI_WAYLAND_STEREO_H
#define UI_WAYLAND_STEREO_H

#include "ui/console.h"

struct wl_display;
struct wl_surface;

typedef struct QemuWaylandStereo QemuWaylandStereo;

QemuWaylandStereo *qemu_wayland_stereo_new(struct wl_display *display,
                                           struct wl_surface *parent);
void qemu_wayland_stereo_free(QemuWaylandStereo *ws);
struct wl_surface *qemu_wayland_stereo_get_parent(QemuWaylandStereo *ws);
void qemu_wayland_stereo_show(QemuWaylandStereo *ws, pixman_image_t *frame,
                              const QemuStereo *stereo,
                              int x, int y, int width, int height);
void qemu_wayland_stereo_hide(QemuWaylandStereo *ws);

#endif /* UI_WAYLAND_STEREO_H */
