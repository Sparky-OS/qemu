/*
 * A picture with its depth map, drawn as the two views of a stereo scanout
 *
 * HDMI 1.4a, Appendix H (a preliminary draft), Figure H-5 puts the 2D picture
 * and its depth one vertical total apart ("L + depth"), and Figure H-6 puts
 * the graphics and their depth under them ("L + depth + graphics +
 * graphics-depth").  The guest tells where each picture is through
 * VIRTIO_GPU_CMD_SET_SCANOUT_STEREO; this turns them into the two views of
 * full side by side with libstereo-dibr, so that no program draws depth of its
 * own (depth content is assembled by the one common library).
 *
 * A depth sample is read as nearness (larger nearer, uniform in parallax), the
 * convention of 2D-plus-depth and MPEG-C Part 3 depth maps
 * (stereo_depth_kind STEREO_DEPTH_NEARNESS); ISO/IEC 23002-3, which HDMI 1.4a
 * Appendix H points to, is not freely available and was not read: that
 * reading is unverified, and the depth maps a person gives the guest decide.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/stereo-depth.h"

#include <math.h>
#include <stereo-dibr.h>

#define DEPTH_BITS 8

typedef struct DepthPlanes {
    uint8_t *data[3];
    int width, height;
} DepthPlanes;

static stereo_dibr *dibr_ctx;
static bool options_read;
static bool options_ok;
static struct stereo_dibr_options options;

static bool rect_in_frame(const QemuRect *r, int w, int h)
{
    return r->x >= 0 && r->y >= 0 && r->width > 0 && r->height > 0 &&
           r->width <= w && r->height <= h &&
           r->x <= w - r->width && r->y <= h - r->height;
}

static bool depth_options(Error **errp)
{
    if (!options_read) {
        const char *text = getenv("QEMU_STEREO_DEPTH_OPTIONS");

        options_read = true;
        stereo_dibr_options_default(&options);
        options_ok = !text || stereo_dibr_options_read(text, &options) > 0;
        if (options_ok) {
            stereo_dibr_options_clamp(&options);
        }
    }
    if (!options_ok) {
        error_setg(errp, "QEMU_STEREO_DEPTH_OPTIONS is not a set of"
                   " libstereo-dibr options: %s",
                   getenv("QEMU_STEREO_DEPTH_OPTIONS"));
        return false;
    }
    return true;
}

static bool packed_format(pixman_image_t *image)
{
    pixman_format_code_t f = pixman_image_get_format(image);

    return f == PIXMAN_a8r8g8b8 || f == PIXMAN_x8r8g8b8;
}

/* The three planes (blue, green, red) of the rectangle of a packed frame. */
static void planes_from_frame(pixman_image_t *frame, const QemuRect *r,
                              DepthPlanes *p)
{
    const uint8_t *base = (const uint8_t *)pixman_image_get_data(frame);
    int stride = pixman_image_get_stride(frame);

    p->width = r->width;
    p->height = r->height;
    for (int c = 0; c < 3; c++) {
        p->data[c] = g_malloc((size_t)r->width * r->height);
    }
    for (int y = 0; y < r->height; y++) {
        const uint8_t *row = base + (size_t)(r->y + y) * stride + r->x * 4;
        for (int x = 0; x < r->width; x++) {
            for (int c = 0; c < 3; c++) {
                p->data[c][(size_t)y * r->width + x] = row[x * 4 + c];
            }
        }
    }
}

static void planes_free(DepthPlanes *p)
{
    for (int c = 0; c < 3; c++) {
        g_free(p->data[c]);
        p->data[c] = NULL;
    }
}

static void planes_alloc(DepthPlanes *p, int w, int h)
{
    p->width = w;
    p->height = h;
    for (int c = 0; c < 3; c++) {
        p->data[c] = g_malloc0((size_t)w * h);
    }
}

static void picture_of(const DepthPlanes *p, struct stereo_dibr_picture *pic)
{
    *pic = (struct stereo_dibr_picture) {
        .width = p->width,
        .height = p->height,
        .nb_planes = 3,
    };
    for (int c = 0; c < 3; c++) {
        pic->plane[c] = (struct stereo_dibr_plane) {
            .data = p->data[c],
            .stride = p->width,
            .bytes = 1,
        };
    }
}

/* The gray of a depth rectangle (its green sample) as a depth map. */
static uint8_t *depth_gray(pixman_image_t *frame, const QemuRect *r)
{
    const uint8_t *base = (const uint8_t *)pixman_image_get_data(frame);
    int stride = pixman_image_get_stride(frame);
    uint8_t *gray = g_malloc((size_t)r->width * r->height);

    for (int y = 0; y < r->height; y++) {
        const uint8_t *row = base + (size_t)(r->y + y) * stride + r->x * 4;
        for (int x = 0; x < r->width; x++) {
            gray[(size_t)y * r->width + x] = row[x * 4 + 1];
        }
    }
    return gray;
}

static void put_view(pixman_image_t *dst, const DepthPlanes *p, int dst_x)
{
    uint8_t *base = (uint8_t *)pixman_image_get_data(dst);
    int stride = pixman_image_get_stride(dst);
    bool alpha = PIXMAN_FORMAT_A(pixman_image_get_format(dst));

    for (int y = 0; y < p->height; y++) {
        uint8_t *row = base + (size_t)y * stride + dst_x * 4;
        for (int x = 0; x < p->width; x++) {
            for (int c = 0; c < 3; c++) {
                row[x * 4 + c] = p->data[c][(size_t)y * p->width + x];
            }
            row[x * 4 + 3] = alpha ? 0xff : 0;
        }
    }
}

bool qemu_stereo_depth_render(pixman_image_t *frame, const QemuStereo *stereo,
                              pixman_image_t *dst, Error **errp)
{
    int fw = pixman_image_get_width(frame);
    int fh = pixman_image_get_height(frame);
    bool graphics = stereo->layout == 6;
    struct stereo_depth_convention conv = {
        .kind = STEREO_DEPTH_NEARNESS,
        .bits = DEPTH_BITS,
    };
    struct stereo_dibr_params params;
    struct stereo_dibr_picture picture, left_pic, right_pic;
    struct stereo_dibr_depth depth;
    DepthPlanes pic = { 0 }, lv = { 0 }, rv = { 0 }, layer = { 0 };
    uint8_t *gray = NULL, *layer_gray = NULL;
    float *table, *layer_table = NULL;
    int w, h, ret;
    bool ok = false;

    if (!qemu_stereo_layout_is_depth(stereo->layout)) {
        error_setg(errp, "stereo layout %u is not a picture with its depth",
                   stereo->layout);
        return false;
    }
    if (!packed_format(frame) || !packed_format(dst)) {
        error_setg(errp, "a picture with its depth needs a 32 bit frame");
        return false;
    }
    if (!rect_in_frame(&stereo->left, fw, fh)) {
        error_setg(errp, "the picture of a picture with its depth is outside"
                   " the frame");
        return false;
    }
    if (!rect_in_frame(&stereo->right, fw, fh)) {
        error_setg(errp, "the guest sent no depth rectangle inside the frame"
                   " for its picture with depth");
        return false;
    }
    w = stereo->left.width;
    h = stereo->left.height;
    if (graphics &&
        (!rect_in_frame(&stereo->graphics, fw, fh) ||
         !rect_in_frame(&stereo->graphics_depth, fw, fh) ||
         stereo->graphics.width != w || stereo->graphics.height != h)) {
        error_setg(errp, "the graphics or their depth are outside the frame,"
                   " or not the picture's size");
        return false;
    }
    if (pixman_image_get_width(dst) < 2 * w || pixman_image_get_height(dst) < h) {
        error_setg(errp, "the buffer is smaller than the two views");
        return false;
    }
    if (!depth_options(errp)) {
        return false;
    }
    if (!dibr_ctx) {
        dibr_ctx = stereo_dibr_alloc();
        if (!dibr_ctx) {
            error_setg(errp, "libstereo-dibr is out of memory");
            return false;
        }
    }

    stereo_dibr_options_apply(&options, &conv, &params);
    table = g_new(float, 1 << DEPTH_BITS);
    if (stereo_depth_parallax_table(&conv, w, table) < 0) {
        error_setg(errp, "libstereo-dibr refused the depth convention");
        g_free(table);
        return false;
    }

    planes_from_frame(frame, &stereo->left, &pic);
    planes_alloc(&lv, w, h);
    planes_alloc(&rv, w, h);
    picture_of(&pic, &picture);
    picture_of(&lv, &left_pic);
    picture_of(&rv, &right_pic);
    gray = depth_gray(frame, &stereo->right);
    depth = (struct stereo_dibr_depth) {
        .data = gray,
        .stride = stereo->right.width,
        .bytes = 1,
        .width = stereo->right.width,
        .height = stereo->right.height,
    };

    if (graphics) {
        struct stereo_dibr_picture layer_pic;
        struct stereo_dibr_depth layer_depth;

        planes_from_frame(frame, &stereo->graphics, &layer);
        picture_of(&layer, &layer_pic);
        layer_gray = depth_gray(frame, &stereo->graphics_depth);
        /*
         * A graphics depth of 0 holds no graphics: the picture shows there.
         * This is an implementation choice, not a clause: Appendix H (a draft)
         * gives the graphics no transparency of their own, and a graphics layer
         * that is opaque everywhere would hide the picture altogether.
         */
        layer_table = g_memdup2(table, sizeof(float) << DEPTH_BITS);
        layer_table[0] = NAN;
        layer_depth = (struct stereo_dibr_depth) {
            .data = layer_gray,
            .stride = stereo->graphics_depth.width,
            .bytes = 1,
            .width = stereo->graphics_depth.width,
            .height = stereo->graphics_depth.height,
        };
        ret = stereo_dibr_render_layer(dibr_ctx, &params, table,
                                       1 << DEPTH_BITS, &picture, &depth,
                                       STEREO_DIBR_LAYER_OVERLAY, &layer_pic,
                                       layer_table, 1 << DEPTH_BITS,
                                       &layer_depth,
                                       &left_pic, &right_pic);
    } else {
        ret = stereo_dibr_render(dibr_ctx, &params, table, 1 << DEPTH_BITS,
                                 &picture, &depth, &left_pic, &right_pic);
    }
    if (ret < 0) {
        error_setg(errp, "libstereo-dibr failed to draw the views (%d)", ret);
    } else {
        put_view(dst, &lv, 0);
        put_view(dst, &rv, w);
        ok = true;
    }

    planes_free(&pic);
    planes_free(&lv);
    planes_free(&rv);
    planes_free(&layer);
    g_free(gray);
    g_free(layer_gray);
    g_free(layer_table);
    g_free(table);
    return ok;
}
