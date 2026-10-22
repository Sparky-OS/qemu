/*
 * Stereo scanouts on a Wayland subsurface
 *
 * The two views of a stereo scanout go to the compositor side by side,
 * left view first, each at the resolution the guest scans it out at, on a
 * subsurface of the display's window, with a viewport that gives it the
 * size one view is shown at.  The views are copied, never scaled: a
 * top-and-bottom 1920x1080 frame becomes one 3840x540 buffer, a side by
 * side (half) one two 960x1080 views, and the compositor resizes them once
 * to the viewport.  The subsurface is declared once as full side by side
 * through libstereo-declare (KWin's stereo content protocol); it takes no
 * input, which goes to the window below it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/memfd.h"
#include "ui/wayland-stereo.h"

#include <wayland-client.h>
#include <stereo-declare.h>
#include "viewporter-client-protocol.h"

#define WAYLAND_STEREO_BUFFERS 2

typedef struct QemuWaylandStereoBuffer {
    struct wl_buffer *buffer;
    pixman_image_t *image;
    void *data;
    size_t size;
    int fd;
    bool busy;
} QemuWaylandStereoBuffer;

struct QemuWaylandStereo {
    struct wl_surface *parent;
    struct wl_surface *surface;
    struct wl_subsurface *subsurface;
    struct wp_viewport *viewport;
    QemuWaylandStereoBuffer buffers[WAYLAND_STEREO_BUFFERS];
    bool declared;
};

/* One connection, the display's, with a queue of our own. */
static struct {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct wp_viewporter *viewporter;
    bool initialized;
    bool supported;
} wl;

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name) && version >= 4) {
        wl.compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface, 4);
    } else if (!strcmp(interface, wl_subcompositor_interface.name)) {
        wl.subcompositor = wl_registry_bind(registry, name,
                                            &wl_subcompositor_interface, 1);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        wl.viewporter = wl_registry_bind(registry, name,
                                         &wp_viewporter_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static bool wayland_stereo_init(struct wl_display *display)
{
    struct wl_display *wrapper;
    struct wl_registry *registry;

    if (wl.initialized) {
        return wl.supported && wl.display == display;
    }
    wl.initialized = true;
    wl.display = display;
    wl.queue = wl_display_create_queue(display);

    wrapper = wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *)wrapper, wl.queue);
    registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    if (stereo_declare_wayland_init(display, wl.queue) < 0) {
        return false;
    }
    /* the globals, then what binding them announces */
    wl_display_roundtrip_queue(display, wl.queue);
    wl_display_roundtrip_queue(display, wl.queue);

    wl.supported = wl.compositor && wl.subcompositor && wl.shm &&
                   wl.viewporter && stereo_supported_wayland() >= 1;
    if (!wl.supported) {
        info_report("wayland: the compositor takes no stereo surfaces, "
                    "stereo scanouts show their left view");
    }
    return wl.supported;
}

QemuWaylandStereo *qemu_wayland_stereo_new(struct wl_display *display,
                                           struct wl_surface *parent)
{
    QemuWaylandStereo *ws;
    struct wl_region *region;

    if (!wayland_stereo_init(display)) {
        return NULL;
    }

    ws = g_new0(QemuWaylandStereo, 1);
    ws->parent = parent;
    ws->surface = wl_compositor_create_surface(wl.compositor);
    ws->subsurface = wl_subcompositor_get_subsurface(wl.subcompositor,
                                                     ws->surface, parent);
    wl_subsurface_set_desync(ws->subsurface);
    region = wl_compositor_create_region(wl.compositor);
    wl_surface_set_input_region(ws->surface, region);
    wl_region_destroy(region);
    ws->viewport = wp_viewporter_get_viewport(wl.viewporter, ws->surface);
    for (int i = 0; i < WAYLAND_STEREO_BUFFERS; i++) {
        ws->buffers[i].fd = -1;
    }
    return ws;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    QemuWaylandStereoBuffer *buf = data;

    buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void buffer_free(QemuWaylandStereoBuffer *buf)
{
    if (buf->buffer) {
        wl_buffer_destroy(buf->buffer);
        pixman_image_unref(buf->image);
        qemu_memfd_free(buf->data, buf->size, buf->fd);
    }
    *buf = (QemuWaylandStereoBuffer) { .fd = -1 };
}

static bool buffer_alloc(QemuWaylandStereoBuffer *buf, int width, int height)
{
    struct wl_shm_pool *pool;
    int stride = width * 4;

    buf->size = (size_t)stride * height;
    buf->data = qemu_memfd_alloc("qemu-stereo", buf->size, 0, &buf->fd, NULL);
    if (!buf->data) {
        *buf = (QemuWaylandStereoBuffer) { .fd = -1 };
        return false;
    }
    pool = wl_shm_create_pool(wl.shm, buf->fd, buf->size);
    buf->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                            WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    wl_buffer_add_listener(buf->buffer, &buffer_listener, buf);
    buf->image = pixman_image_create_bits(PIXMAN_x8r8g8b8, width, height,
                                          buf->data, stride);
    return true;
}

static QemuWaylandStereoBuffer *buffer_get(QemuWaylandStereo *ws,
                                           int width, int height)
{
    for (int i = 0; i < WAYLAND_STEREO_BUFFERS; i++) {
        QemuWaylandStereoBuffer *buf = &ws->buffers[i];

        if (buf->busy) {
            continue;
        }
        if (buf->buffer &&
            (pixman_image_get_width(buf->image) != width ||
             pixman_image_get_height(buf->image) != height)) {
            buffer_free(buf);
        }
        if (!buf->buffer && !buffer_alloc(buf, width, height)) {
            return NULL;
        }
        return buf;
    }
    return NULL;
}

/* Copies one view of the frame, at its own size, to dst_x in dst. */
static void copy_view(pixman_image_t *frame, const QemuRect *view,
                      pixman_image_t *dst, int dst_x)
{
    pixman_image_composite(PIXMAN_OP_SRC, frame, NULL, dst,
                           view->x, view->y, 0, 0, dst_x, 0,
                           view->width, view->height);
}

void qemu_wayland_stereo_show(QemuWaylandStereo *ws, pixman_image_t *frame,
                              const QemuStereo *stereo,
                              int x, int y, int width, int height)
{
    QemuWaylandStereoBuffer *buf;

    /* every 3D structure gives both views one size */
    if (stereo->left.width != stereo->right.width ||
        stereo->left.height != stereo->right.height) {
        return;
    }

    wl_display_dispatch_queue_pending(wl.display, wl.queue);
    buf = buffer_get(ws, stereo->left.width * 2, stereo->left.height);
    if (!buf) {
        return;
    }
    copy_view(frame, &stereo->left, buf->image, 0);
    copy_view(frame, &stereo->right, buf->image, stereo->left.width);

    if (!ws->declared) {
        stereo_declare_wayland(ws->surface, STEREO_SBS_FULL,
                               STEREO_CLASS_NONE, STEREO_SUBCLASS_UNSPECIFIED);
        ws->declared = true;
    }
    wl_subsurface_set_position(ws->subsurface, x, y);
    wp_viewport_set_destination(ws->viewport, width, height);
    wl_surface_attach(ws->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(ws->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(ws->surface);
    buf->busy = true;
    wl_display_flush(wl.display);
}

void qemu_wayland_stereo_hide(QemuWaylandStereo *ws)
{
    if (!ws->declared) {
        return;
    }
    stereo_remove_wayland(ws->surface);
    ws->declared = false;
    wl_surface_attach(ws->surface, NULL, 0, 0);
    wl_surface_commit(ws->surface);
    wl_display_flush(wl.display);
}

struct wl_surface *qemu_wayland_stereo_get_parent(QemuWaylandStereo *ws)
{
    return ws->parent;
}

void qemu_wayland_stereo_free(QemuWaylandStereo *ws)
{
    if (!ws) {
        return;
    }
    qemu_wayland_stereo_hide(ws);
    for (int i = 0; i < WAYLAND_STEREO_BUFFERS; i++) {
        buffer_free(&ws->buffers[i]);
    }
    wp_viewport_destroy(ws->viewport);
    wl_subsurface_destroy(ws->subsurface);
    wl_surface_destroy(ws->surface);
    wl_display_flush(wl.display);
    g_free(ws);
}
