/*
 * A picture with its depth map, drawn as the two views of a stereo scanout
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_STEREO_DEPTH_H
#define UI_STEREO_DEPTH_H

#include "ui/console.h"

/* The layouts of a picture with its depth (the virtio-gpu protocol's). */
static inline bool qemu_stereo_layout_is_depth(uint32_t layout)
{
    return layout == 5 /* VIRTIO_GPU_STEREO_L_DEPTH */ ||
           layout == 6 /* VIRTIO_GPU_STEREO_L_DEPTH_GFX_GFX_DEPTH */;
}

/*
 * Draws the picture of @stereo (its @left rectangle of @frame) and its depth
 * (@right) as two views, the left view first, each at the picture's size, into
 * @dst, a pixman image of the frame's format twice the picture's width and as
 * high.  The second layout also has its graphics and their depth drawn over
 * the picture.  The drawing is libstereo-dibr's, the library every program
 * that turns depth into stereo uses, with its default options, or the set in
 * the environment variable QEMU_STEREO_DEPTH_OPTIONS (the text of
 * stereo_dibr_options_write(), e.g. "strength=40 zero=0.5").
 *
 * Returns false and sets @errp when it cannot: a frame of another pixel
 * format, a rectangle outside the frame, graphics of another size than the
 * picture, options the library does not read, or the library failing.
 */
bool qemu_stereo_depth_render(pixman_image_t *frame, const QemuStereo *stereo,
                              pixman_image_t *dst, Error **errp);

#endif /* UI_STEREO_DEPTH_H */
