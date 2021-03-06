/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define SPICE_LOG_DOMAIN "SpiceWorker"

#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <inttypes.h>
#include <glib.h>

#include <spice/protocol.h>
#include <spice/qxl_dev.h>
#include "common/lz.h"
#include "common/rect.h"
#include "common/region.h"
#include "common/ring.h"

#include "display-channel.h"
#include "stream.h"

#include "spice.h"
#include "red_worker.h"
#include "spice_timer_queue.h"
#include "cursor-channel.h"
#include "tree.h"

#define CMD_RING_POLL_TIMEOUT 10 //milli
#define CMD_RING_POLL_RETRIES 200

#define MAX_EVENT_SOURCES 20
#define INF_EVENT_WAIT ~0

struct SpiceWatch {
    struct RedWorker *worker;
    SpiceWatchFunc watch_func;
    void *watch_func_opaque;
};

struct RedWorker {
    pthread_t thread;
    clockid_t clockid;
    QXLInstance *qxl;
    RedDispatcher *red_dispatcher;
    int running;
    struct pollfd poll_fds[MAX_EVENT_SOURCES];
    struct SpiceWatch watches[MAX_EVENT_SOURCES];
    unsigned int event_timeout;

    DisplayChannel *display_channel;
    uint32_t display_poll_tries;

    CursorChannel *cursor_channel;
    uint32_t cursor_poll_tries;

    RedMemSlotInfo mem_slots;

    SpiceImageCompression image_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;

    uint32_t process_commands_generation;
#ifdef RED_STATISTICS
    StatNodeRef stat;
    uint64_t *wakeup_counter;
    uint64_t *command_counter;
#endif

    int driver_cap_monitors_config;

    FILE *record_fd;
};

QXLInstance* red_worker_get_qxl(RedWorker *worker)
{
    spice_return_val_if_fail(worker != NULL, NULL);

    return worker->qxl;
}

RedMemSlotInfo* red_worker_get_memslot(RedWorker *worker)
{
    spice_return_val_if_fail(worker != NULL, NULL);

    return &worker->mem_slots;
}

static int display_is_connected(RedWorker *worker)
{
    return (worker->display_channel && red_channel_is_connected(
        &worker->display_channel->common.base));
}

static int validate_drawable_bbox(DisplayChannel *display, RedDrawable *drawable)
{
        DrawContext *context;
        uint32_t surface_id = drawable->surface_id;

        /* surface_id must be validated before calling into
         * validate_drawable_bbox
         */
        if (!validate_surface(display, drawable->surface_id)) {
            return FALSE;
        }
        context = &display->surfaces[surface_id].context;

        if (drawable->bbox.top < 0)
                return FALSE;
        if (drawable->bbox.left < 0)
                return FALSE;
        if (drawable->bbox.bottom < 0)
                return FALSE;
        if (drawable->bbox.right < 0)
                return FALSE;
        if (drawable->bbox.bottom > context->height)
                return FALSE;
        if (drawable->bbox.right > context->width)
                return FALSE;

        return TRUE;
}

static int cursor_is_connected(RedWorker *worker)
{
    return worker->cursor_channel &&
        red_channel_is_connected(RED_CHANNEL(worker->cursor_channel));
}

static uint8_t *common_alloc_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    CommonChannel *common = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base);

    /* SPICE_MSGC_MIGRATE_DATA is the only client message whose size is dynamic */
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        return spice_malloc(size);
    }

    if (size > CHANNEL_RECEIVE_BUF_SIZE) {
        spice_critical("unexpected message size %u (max is %d)", size, CHANNEL_RECEIVE_BUF_SIZE);
        return NULL;
    }
    return common->recv_buf;
}

static void common_release_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size,
                                    uint8_t* msg)
{
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        free(msg);
    }
}

static inline void set_surface_release_info(QXLReleaseInfoExt *release_info_ext,
                                            QXLReleaseInfo *release_info, uint32_t group_id)
{
    release_info_ext->info = release_info;
    release_info_ext->group_id = group_id;
}

void red_drawable_unref(RedWorker *worker, RedDrawable *red_drawable,
                        uint32_t group_id)
{
    QXLReleaseInfoExt release_info_ext;

    if (--red_drawable->refs) {
        return;
    }
    worker->display_channel->red_drawable_count--;
    release_info_ext.group_id = group_id;
    release_info_ext.info = red_drawable->release_info;
    worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
    red_put_drawable(red_drawable);
    free(red_drawable);
}

static void red_get_area(DisplayChannel *display, int surface_id, const SpiceRect *area,
                         uint8_t *dest, int dest_stride, int update)
{
    SpiceCanvas *canvas;
    RedSurface *surface;

    surface = &display->surfaces[surface_id];
    if (update) {
        display_channel_draw(display, area, surface_id);
    }

    canvas = surface->context.canvas;
    canvas->ops->read_bits(canvas, dest, dest_stride, area);
}

static inline int red_handle_self_bitmap(RedWorker *worker, Drawable *drawable)
{
    DisplayChannel *display = worker->display_channel;
    SpiceImage *image;
    int32_t width;
    int32_t height;
    uint8_t *dest;
    int dest_stride;
    RedSurface *surface;
    int bpp;
    int all_set;
    RedDrawable *red_drawable = drawable->red_drawable;

    if (!red_drawable->self_bitmap) {
        return TRUE;
    }

    surface = &display->surfaces[drawable->surface_id];

    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;

    width = red_drawable->self_bitmap_area.right
            - red_drawable->self_bitmap_area.left;
    height = red_drawable->self_bitmap_area.bottom
            - red_drawable->self_bitmap_area.top;
    dest_stride = SPICE_ALIGN(width * bpp, 4);

    image = spice_new0(SpiceImage, 1);
    image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    image->descriptor.flags = 0;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_RED, display_channel_generate_uid(display));
    image->u.bitmap.flags = surface->context.top_down ? SPICE_BITMAP_FLAGS_TOP_DOWN : 0;
    image->u.bitmap.format = spice_bitmap_from_surface_type(surface->context.format);
    image->u.bitmap.stride = dest_stride;
    image->descriptor.width = image->u.bitmap.x = width;
    image->descriptor.height = image->u.bitmap.y = height;
    image->u.bitmap.palette = NULL;

    dest = (uint8_t *)spice_malloc_n(height, dest_stride);
    image->u.bitmap.data = spice_chunks_new_linear(dest, height * dest_stride);
    image->u.bitmap.data->flags |= SPICE_CHUNKS_FLAGS_FREE;

    red_get_area(display, drawable->surface_id,
                 &red_drawable->self_bitmap_area, dest, dest_stride, TRUE);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(display, drawable->surface_id) &&
        image->u.bitmap.format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(width, height, dest_stride, dest, &all_set)) {
        if (all_set) {
            image->descriptor.flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            image->u.bitmap.format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    red_drawable->self_bitmap_image = image;
    return TRUE;
}

static Drawable *get_drawable(RedWorker *worker, uint8_t effect, RedDrawable *red_drawable,
                              uint32_t group_id)
{
    DisplayChannel *display = worker->display_channel;
    Drawable *drawable;
    int x;

    if (!validate_drawable_bbox(display, red_drawable)) {
        return NULL;
    }
    for (x = 0; x < 3; ++x) {
        if (red_drawable->surface_deps[x] != -1
            && !validate_surface(display, red_drawable->surface_deps[x])) {
            return NULL;
        }
    }

    drawable = display_channel_drawable_try_new(display, group_id, worker->process_commands_generation);
    if (!drawable) {
        return NULL;
    }

    drawable->tree_item.effect = effect;
    drawable->red_drawable = red_drawable_ref(red_drawable);

    drawable->surface_id = red_drawable->surface_id;
    memcpy(drawable->surface_deps, red_drawable->surface_deps, sizeof(drawable->surface_deps));

    return drawable;
}

static inline void add_to_surface_dependency(DisplayChannel *display, int depend_on_surface_id,
                                             DependItem *depend_item, Drawable *drawable)
{
    RedSurface *surface;

    if (depend_on_surface_id == -1) {
        depend_item->drawable = NULL;
        return;
    }

    surface = &display->surfaces[depend_on_surface_id];

    depend_item->drawable = drawable;
    ring_add(&surface->depend_on_me, &depend_item->ring_item);
}

static inline int red_handle_surfaces_dependencies(DisplayChannel *display, Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        // surface self dependency is handled by shadows in "current", or by
        // handle_self_bitmap
        if (drawable->surface_deps[x] != drawable->surface_id) {
            add_to_surface_dependency(display, drawable->surface_deps[x],
                                      &drawable->depend_items[x], drawable);

            if (drawable->surface_deps[x] == 0) {
                QRegion depend_region;
                region_init(&depend_region);
                region_add(&depend_region, &drawable->red_drawable->surfaces_rects[x]);
                detach_streams_behind(display, &depend_region, NULL);
            }
        }
    }

    return TRUE;
}

static inline void red_inc_surfaces_drawable_dependencies(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;
    RedSurface *surface;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id == -1) {
            continue;
        }
        surface = &display->surfaces[surface_id];
        surface->refs++;
    }
}

static inline void red_process_draw(RedWorker *worker, RedDrawable *red_drawable,
                                    uint32_t group_id)
{
    DisplayChannel *display = worker->display_channel;
    int surface_id;
    Drawable *drawable = get_drawable(worker, red_drawable->effect, red_drawable, group_id);

    if (!drawable) {
        return;
    }

    red_drawable->mm_time = reds_get_mm_time();
    surface_id = drawable->surface_id;

    display->surfaces[surface_id].refs++;

    region_add(&drawable->tree_item.base.rgn, &red_drawable->bbox);

    if (red_drawable->clip.type == SPICE_CLIP_TYPE_RECTS) {
        QRegion rgn;

        region_init(&rgn);
        region_add_clip_rects(&rgn, red_drawable->clip.rects);
        region_and(&drawable->tree_item.base.rgn, &rgn);
        region_destroy(&rgn);
    }

    /*
        surface->refs is affected by a drawable (that is
        dependent on the surface) as long as the drawable is alive.
        However, surface->depend_on_me is affected by a drawable only
        as long as it is in the current tree (hasn't been rendered yet).
    */
    red_inc_surfaces_drawable_dependencies(worker->display_channel, drawable);

    if (region_is_empty(&drawable->tree_item.base.rgn)) {
        goto cleanup;
    }

    if (!red_handle_self_bitmap(worker, drawable)) {
        goto cleanup;
    }

    draw_depend_on_me(display, surface_id);

    if (!red_handle_surfaces_dependencies(worker->display_channel, drawable)) {
        goto cleanup;
    }

    display_channel_add_drawable(worker->display_channel, drawable);

cleanup:
    display_channel_drawable_unref(display, drawable);
}


static inline void red_process_surface(RedWorker *worker, RedSurfaceCmd *surface,
                                       uint32_t group_id, int loadvm)
{
    DisplayChannel *display = worker->display_channel;
    uint32_t surface_id;
    RedSurface *red_surface;
    uint8_t *data;

    surface_id = surface->surface_id;
    if SPICE_UNLIKELY(surface_id >= display->n_surfaces) {
        goto exit;
    }

    red_surface = &display->surfaces[surface_id];

    switch (surface->type) {
    case QXL_SURFACE_CMD_CREATE: {
        uint32_t height = surface->u.surface_create.height;
        int32_t stride = surface->u.surface_create.stride;
        int reloaded_surface = loadvm || (surface->flags & QXL_SURF_FLAG_KEEP_DATA);

        if (red_surface->refs) {
            spice_warning("avoiding creating a surface twice");
            break;
        }
        data = surface->u.surface_create.data;
        if (stride < 0) {
            data -= (int32_t)(stride * (height - 1));
        }
        display_channel_create_surface(worker->display_channel, surface_id, surface->u.surface_create.width,
                                       height, stride, surface->u.surface_create.format, data,
                                       reloaded_surface,
                                       // reloaded surfaces will be sent on demand
                                       !reloaded_surface);
        set_surface_release_info(&red_surface->create, surface->release_info, group_id);
        break;
    }
    case QXL_SURFACE_CMD_DESTROY:
        if (!red_surface->refs) {
            spice_warning("avoiding destroying a surface twice");
            break;
        }
        set_surface_release_info(&red_surface->destroy, surface->release_info, group_id);
        display_channel_destroy_surface(display, surface_id);
        break;
    default:
        spice_warn_if_reached();
    };
exit:
    red_put_surface_cmd(surface);
    free(surface);
}

static int red_process_cursor(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }

    *ring_is_empty = FALSE;
    while (!cursor_is_connected(worker) ||
           red_channel_min_pipe_size(RED_CHANNEL(worker->cursor_channel)) <= max_pipe_size) {
        if (!worker->qxl->st->qif->get_cursor_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;
            if (worker->cursor_poll_tries < CMD_RING_POLL_RETRIES) {
                worker->cursor_poll_tries++;
                worker->event_timeout = MIN(worker->event_timeout, CMD_RING_POLL_TIMEOUT);
                return n;
            }
            if (worker->cursor_poll_tries > CMD_RING_POLL_RETRIES ||
                worker->qxl->st->qif->req_cursor_notification(worker->qxl)) {
                worker->cursor_poll_tries++;
                return n;
            }
            continue;
        }
        worker->cursor_poll_tries = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_CURSOR: {
            RedCursorCmd *cursor = spice_new0(RedCursorCmd, 1);

            if (red_get_cursor_cmd(&worker->mem_slots, ext_cmd.group_id,
                                    cursor, ext_cmd.cmd.data)) {
                free(cursor);
                break;
            }

            cursor_channel_process_cmd(worker->cursor_channel, cursor, ext_cmd.group_id);
            break;
        }
        default:
            spice_warning("bad command type");
        }
        n++;
    }
    return n;
}

static RedDrawable *red_drawable_new(RedWorker *worker)
{
    RedDrawable * red = spice_new0(RedDrawable, 1);

    red->refs = 1;
    worker->display_channel->red_drawable_count++;

    return red;
}

static int red_process_commands(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;
    uint64_t start = red_get_monotonic_time();

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }

    worker->process_commands_generation++;
    *ring_is_empty = FALSE;
    while (!display_is_connected(worker) ||
           // TODO: change to average pipe size?
           red_channel_min_pipe_size(RED_CHANNEL(worker->display_channel)) <= max_pipe_size) {
        if (!worker->qxl->st->qif->get_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;;
            if (worker->display_poll_tries < CMD_RING_POLL_RETRIES) {
                worker->display_poll_tries++;
                worker->event_timeout = MIN(worker->event_timeout, CMD_RING_POLL_TIMEOUT);
                return n;
            }
            if (worker->display_poll_tries > CMD_RING_POLL_RETRIES ||
                         worker->qxl->st->qif->req_cmd_notification(worker->qxl)) {
                worker->display_poll_tries++;
                return n;
            }
            continue;
        }

        if (worker->record_fd)
            red_record_qxl_command(worker->record_fd, &worker->mem_slots, ext_cmd,
                                   stat_now(worker->clockid));

        stat_inc_counter(worker->command_counter, 1);
        worker->display_poll_tries = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_DRAW: {
            RedDrawable *red_drawable = red_drawable_new(worker); // returns with 1 ref

            if (!red_get_drawable(&worker->mem_slots, ext_cmd.group_id,
                                 red_drawable, ext_cmd.cmd.data, ext_cmd.flags)) {
                red_process_draw(worker, red_drawable, ext_cmd.group_id);
            }
            // release the red_drawable
            red_drawable_unref(worker, red_drawable, ext_cmd.group_id);
            break;
        }
        case QXL_CMD_UPDATE: {
            RedUpdateCmd update;
            QXLReleaseInfoExt release_info_ext;

            if (red_get_update_cmd(&worker->mem_slots, ext_cmd.group_id,
                                   &update, ext_cmd.cmd.data)) {
                break;
            }
            if (!validate_surface(worker->display_channel, update.surface_id)) {
                spice_warning("Invalid surface in QXL_CMD_UPDATE");
                break;
            }
            display_channel_draw(worker->display_channel, &update.area, update.surface_id);
            worker->qxl->st->qif->notify_update(worker->qxl, update.update_id);
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = update.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_update_cmd(&update);
            break;
        }
        case QXL_CMD_MESSAGE: {
            RedMessage message;
            QXLReleaseInfoExt release_info_ext;

            if (red_get_message(&worker->mem_slots, ext_cmd.group_id,
                                &message, ext_cmd.cmd.data)) {
                break;
            }
#ifdef DEBUG
            /* alert: accessing message.data is insecure */
            spice_warning("MESSAGE: %s", message.data);
#endif
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = message.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_message(&message);
            break;
        }
        case QXL_CMD_SURFACE: {
            RedSurfaceCmd *surface = spice_new0(RedSurfaceCmd, 1);

            if (red_get_surface_cmd(&worker->mem_slots, ext_cmd.group_id,
                                    surface, ext_cmd.cmd.data)) {
                free(surface);
                break;
            }
            red_process_surface(worker, surface, ext_cmd.group_id, FALSE);
            break;
        }
        default:
            spice_error("bad command type");
        }
        n++;
        if ((worker->display_channel &&
             red_channel_all_blocked(&worker->display_channel->common.base))
            || red_get_monotonic_time() - start > 10 * 1000 * 1000) {
            worker->event_timeout = 0;
            return n;
        }
    }
    return n;
}

static void red_push(RedWorker *worker)
{
    if (worker->cursor_channel) {
        red_channel_push(RED_CHANNEL(worker->cursor_channel));
    }
    if (worker->display_channel) {
        red_channel_push(RED_CHANNEL(worker->display_channel));
    }
}

void red_disconnect_all_display_TODO_remove_me(RedChannel *channel)
{
    // TODO: we need to record the client that actually causes the timeout. So
    // we need to check the locations of the various pipe heads when counting,
    // and disconnect only those/that.
    if (!channel) {
        return;
    }
    red_channel_apply_clients(channel, red_channel_client_disconnect);
}

static void red_migrate_display(DisplayChannel *display, RedChannelClient *rcc)
{
    /* We need to stop the streams, and to send upgrade_items to the client.
     * Otherwise, (1) the client might display lossy regions that we don't track
     * (streams are not part of the migration data) (2) streams_timeout may occur
     * after the MIGRATE message has been sent. This can result in messages
     * being sent to the client after MSG_MIGRATE and before MSG_MIGRATE_DATA (e.g.,
     * STREAM_CLIP, STREAM_DESTROY, DRAW_COPY)
     * No message besides MSG_MIGRATE_DATA should be sent after MSG_MIGRATE.
     * Notice that detach_and_stop_streams won't lead to any dev ram changes, since
     * handle_dev_stop already took care of releasing all the dev ram resources.
     */
    stream_detach_and_stop(display);
    if (red_channel_client_is_connected(rcc)) {
        red_channel_client_default_migrate(rcc);
    }
}

static void flush_display_commands(RedWorker *worker)
{
    RedChannel *display_red_channel = RED_CHANNEL(worker->display_channel);

    for (;;) {
        uint64_t end_time;
        int ring_is_empty;

        red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(RED_CHANNEL(worker->display_channel));
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_get_monotonic_time() + DISPLAY_CLIENT_TIMEOUT;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(RED_CHANNEL(worker->display_channel));
            if (!display_is_connected(worker) ||
                red_channel_max_pipe_size(display_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->display_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            // TODO: MC: the whole timeout will break since it takes lowest timeout, should
            // do it client by client.
            if (red_get_monotonic_time() >= end_time) {
                spice_warning("update timeout");
                red_disconnect_all_display_TODO_remove_me(channel);
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

static void flush_cursor_commands(RedWorker *worker)
{
    RedChannel *cursor_red_channel = RED_CHANNEL(worker->cursor_channel);

    for (;;) {
        uint64_t end_time;
        int ring_is_empty = FALSE;

        red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(RED_CHANNEL(worker->cursor_channel));
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_get_monotonic_time() + DISPLAY_CLIENT_TIMEOUT;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(RED_CHANNEL(worker->cursor_channel));
            if (!cursor_is_connected(worker)
                || red_channel_min_pipe_size(cursor_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->cursor_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            if (red_get_monotonic_time() >= end_time) {
                spice_warning("flush cursor timeout");
                cursor_channel_disconnect(worker->cursor_channel);
                worker->cursor_channel = NULL;
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

// TODO: on timeout, don't disconnect all channels immediatly - try to disconnect the slowest ones
// first and maybe turn timeouts to several timeouts in order to disconnect channels gradually.
// Should use disconnect or shutdown?
static void flush_all_qxl_commands(RedWorker *worker)
{
    flush_display_commands(worker);
    flush_cursor_commands(worker);
}

static int common_channel_config_socket(RedChannelClient *rcc)
{
    RedClient *client = red_channel_client_get_client(rcc);
    MainChannelClient *mcc = red_client_get_main(client);
    RedsStream *stream = red_channel_client_get_stream(rcc);
    CommonChannelClient *ccc = COMMON_CHANNEL_CLIENT(rcc);
    int flags;
    int delay_val;

    if ((flags = fcntl(stream->socket, F_GETFL)) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return FALSE;
    }

    if (fcntl(stream->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return FALSE;
    }

    // TODO - this should be dynamic, not one time at channel creation
    ccc->is_low_bandwidth = main_channel_client_is_low_bandwidth(mcc);
    delay_val = ccc->is_low_bandwidth ? 0 : 1;
    /* FIXME: Using Nagle's Algorithm can lead to apparent delays, depending
     * on the delayed ack timeout on the other side.
     * Instead of using Nagle's, we need to implement message buffering on
     * the application level.
     * see: http://www.stuartcheshire.org/papers/NagleDelayedAck/
     */
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val,
                   sizeof(delay_val)) == -1) {
        if (errno != ENOTSUP) {
            spice_warning("setsockopt failed, %s", strerror(errno));
        }
    }
    return TRUE;
}

static void worker_watch_update_mask(SpiceWatch *watch, int event_mask)
{
    struct RedWorker *worker;
    int i;

    if (!watch) {
        return;
    }

    worker = watch->worker;
    i = watch - worker->watches;

    worker->poll_fds[i].events = 0;
    if (event_mask & SPICE_WATCH_EVENT_READ) {
        worker->poll_fds[i].events |= POLLIN;
    }
    if (event_mask & SPICE_WATCH_EVENT_WRITE) {
        worker->poll_fds[i].events |= POLLOUT;
    }
}

static SpiceWatch *worker_watch_add(int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    /* Since we are a channel core implementation, we always get called from
       red_channel_client_create(), so opaque always is our rcc */
    RedChannelClient *rcc = opaque;
    struct RedWorker *worker;
    int i;

    /* Since we are called from red_channel_client_create()
       CommonChannelClient->worker has not been set yet! */
    worker = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base)->worker;

    /* Search for a free slot in our poll_fds & watches arrays */
    for (i = 0; i < MAX_EVENT_SOURCES; i++) {
        if (worker->poll_fds[i].fd == -1) {
            break;
        }
    }
    if (i == MAX_EVENT_SOURCES) {
        spice_warning("could not add a watch for channel type %u id %u",
                      rcc->channel->type, rcc->channel->id);
        return NULL;
    }

    worker->poll_fds[i].fd = fd;
    worker->watches[i].worker = worker;
    worker->watches[i].watch_func = func;
    worker->watches[i].watch_func_opaque = opaque;
    worker_watch_update_mask(&worker->watches[i], event_mask);

    return &worker->watches[i];
}

static void worker_watch_remove(SpiceWatch *watch)
{
    if (!watch) {
        return;
    }

    /* Note we don't touch the poll_fd here, to avoid the
       poll_fds/watches table entry getting re-used in the same
       red_worker_main loop over the fds as it is removed.

       This is done because re-using it while events were pending on
       the fd previously occupying the slot would lead to incorrectly
       calling the watch_func for the new fd. */
    memset(watch, 0, sizeof(SpiceWatch));
}

SpiceCoreInterface worker_core = {
    .timer_add = spice_timer_queue_add,
    .timer_start = spice_timer_set,
    .timer_cancel = spice_timer_cancel,
    .timer_remove = spice_timer_remove,

    .watch_update_mask = worker_watch_update_mask,
    .watch_add = worker_watch_add,
    .watch_remove = worker_watch_remove,
};

CommonChannelClient *common_channel_new_client(CommonChannel *common,
                                               int size,
                                               RedClient *client,
                                               RedsStream *stream,
                                               int mig_target,
                                               int monitor_latency,
                                               uint32_t *common_caps,
                                               int num_common_caps,
                                               uint32_t *caps,
                                               int num_caps)
{
    RedChannelClient *rcc =
        red_channel_client_create(size, &common->base, client, stream, monitor_latency,
                                  num_common_caps, common_caps, num_caps, caps);
    if (!rcc) {
        return NULL;
    }
    CommonChannelClient *common_cc = (CommonChannelClient*)rcc;
    common_cc->worker = common->worker;
    common_cc->id = common->worker->qxl->id;
    common->during_target_migrate = mig_target;

    // TODO: move wide/narrow ack setting to red_channel.
    red_channel_client_ack_set_client_window(rcc,
        common_cc->is_low_bandwidth ?
        WIDE_CLIENT_ACK_WINDOW : NARROW_CLIENT_ACK_WINDOW);
    return common_cc;
}


RedChannel *red_worker_new_channel(RedWorker *worker, int size,
                                   const char *name,
                                   uint32_t channel_type, int migration_flags,
                                   ChannelCbs *channel_cbs,
                                   channel_handle_parsed_proc handle_parsed)
{
    RedChannel *channel = NULL;
    CommonChannel *common;

    spice_return_val_if_fail(worker, NULL);
    spice_return_val_if_fail(channel_cbs, NULL);
    spice_return_val_if_fail(!channel_cbs->config_socket, NULL);
    spice_return_val_if_fail(!channel_cbs->alloc_recv_buf, NULL);
    spice_return_val_if_fail(!channel_cbs->release_recv_buf, NULL);

    channel_cbs->config_socket = common_channel_config_socket;
    channel_cbs->alloc_recv_buf = common_alloc_recv_buf;
    channel_cbs->release_recv_buf = common_release_recv_buf;

    channel = red_channel_create_parser(size, &worker_core,
                                        channel_type, worker->qxl->id,
                                        TRUE /* handle_acks */,
                                        spice_get_client_channel_parser(channel_type, NULL),
                                        handle_parsed,
                                        channel_cbs,
                                        migration_flags);
    spice_return_val_if_fail(channel, NULL);
    red_channel_set_stat_node(channel, stat_add_node(worker->stat, name, TRUE));

    common = (CommonChannel *)channel;
    common->worker = worker;
    return channel;
}

static void guest_set_client_capabilities(RedWorker *worker)
{
    int i;
    DisplayChannelClient *dcc;
    RedChannelClient *rcc;
    RingItem *link, *next;
    uint8_t caps[SPICE_CAPABILITIES_SIZE] = { 0 };
    int caps_available[] = {
        SPICE_DISPLAY_CAP_SIZED_STREAM,
        SPICE_DISPLAY_CAP_MONITORS_CONFIG,
        SPICE_DISPLAY_CAP_COMPOSITE,
        SPICE_DISPLAY_CAP_A8_SURFACE,
    };

    if (worker->qxl->st->qif->base.major_version < 3 ||
        (worker->qxl->st->qif->base.major_version == 3 &&
        worker->qxl->st->qif->base.minor_version < 2) ||
        !worker->qxl->st->qif->set_client_capabilities) {
        return;
    }
#define SET_CAP(a,c)                                                    \
        ((a)[(c) / 8] |= (1 << ((c) % 8)))

#define CLEAR_CAP(a,c)                                                  \
        ((a)[(c) / 8] &= ~(1 << ((c) % 8)))

    if (!worker->running) {
        return;
    }
    if ((worker->display_channel == NULL) ||
        (RED_CHANNEL(worker->display_channel)->clients_num == 0)) {
        worker->qxl->st->qif->set_client_capabilities(worker->qxl, FALSE, caps);
    } else {
        // Take least common denominator
        for (i = 0 ; i < sizeof(caps_available) / sizeof(caps_available[0]); ++i) {
            SET_CAP(caps, caps_available[i]);
        }
        DCC_FOREACH_SAFE(link, next, dcc, RED_CHANNEL(worker->display_channel)) {
            rcc = (RedChannelClient *)dcc;
            for (i = 0 ; i < sizeof(caps_available) / sizeof(caps_available[0]); ++i) {
                if (!red_channel_client_test_remote_cap(rcc, caps_available[i]))
                    CLEAR_CAP(caps, caps_available[i]);
            }
        }
        worker->qxl->st->qif->set_client_capabilities(worker->qxl, TRUE, caps);
    }
}

static void handle_new_display_channel(RedWorker *worker, RedClient *client, RedsStream *stream,
                                       int migrate,
                                       uint32_t *common_caps, int num_common_caps,
                                       uint32_t *caps, int num_caps)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc;

    spice_return_if_fail(worker->display_channel);

    display_channel = worker->display_channel;
    spice_info("add display channel client");
    dcc = dcc_new(display_channel, client, stream, migrate,
                  common_caps, num_common_caps, caps, num_caps,
                  worker->image_compression, worker->jpeg_state, worker->zlib_glz_state);
    if (!dcc) {
        return;
    }

    if (dcc->jpeg_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_jpeg = dcc->common.is_low_bandwidth;
    } else {
        display_channel->enable_jpeg = (dcc->jpeg_state == SPICE_WAN_COMPRESSION_ALWAYS);
    }

    if (dcc->zlib_glz_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_zlib_glz_wrap = dcc->common.is_low_bandwidth;
    } else {
        display_channel->enable_zlib_glz_wrap = (dcc->zlib_glz_state ==
                                                 SPICE_WAN_COMPRESSION_ALWAYS);
    }
    spice_info("jpeg %s", display_channel->enable_jpeg ? "enabled" : "disabled");
    spice_info("zlib-over-glz %s", display_channel->enable_zlib_glz_wrap ? "enabled" : "disabled");

    guest_set_client_capabilities(worker);
    dcc_start(dcc);
}

static void cursor_connect(RedWorker *worker, RedClient *client, RedsStream *stream,
                           int migrate,
                           uint32_t *common_caps, int num_common_caps,
                           uint32_t *caps, int num_caps)
{
    CursorChannel *channel = worker->cursor_channel;
    CursorChannelClient *ccc;

    spice_return_if_fail(channel != NULL);

    spice_info("add cursor channel client");
    ccc = cursor_channel_client_new(channel, client, stream,
                                    migrate,
                                    common_caps, num_common_caps,
                                    caps, num_caps);
    spice_return_if_fail(ccc != NULL);

    RedChannelClient *rcc = RED_CHANNEL_CLIENT(ccc);
    red_channel_client_ack_zero_messages_window(rcc);
    red_channel_client_push_set_ack(rcc);

    // TODO: why do we check for context.canvas? defer this to after display cc is connected
    // and test it's canvas? this is just a test to see if there is an active renderer?
    if (display_channel_surface_has_canvas(worker->display_channel, 0))
        cursor_channel_init(channel, ccc);
}

static void handle_dev_update_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdateAsync *msg = payload;
    QXLRect *qxl_dirty_rects = NULL;
    uint32_t num_dirty_rects = 0;

    spice_return_if_fail(worker->running);
    spice_return_if_fail(worker->qxl->st->qif->update_area_complete);

    flush_display_commands(worker);
    display_channel_update(worker->display_channel,
                           msg->surface_id, &msg->qxl_area, msg->clear_dirty_region,
                           &qxl_dirty_rects, &num_dirty_rects);

    worker->qxl->st->qif->update_area_complete(worker->qxl, msg->surface_id,
                                                qxl_dirty_rects, num_dirty_rects);
    free(qxl_dirty_rects);
}

static void handle_dev_update(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdate *msg = payload;

    spice_return_if_fail(worker->running);

    flush_display_commands(worker);
    display_channel_update(worker->display_channel,
                           msg->surface_id, msg->qxl_area, msg->clear_dirty_region,
                           &msg->qxl_dirty_rects, &msg->num_dirty_rects);
}

static void handle_dev_del_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageDelMemslot *msg = payload;
    uint32_t slot_id = msg->slot_id;
    uint32_t slot_group_id = msg->slot_group_id;

    red_memslot_info_del_slot(&worker->mem_slots, slot_group_id, slot_id);
}

static void handle_dev_destroy_surface_wait(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWait *msg = payload;
    RedWorker *worker = opaque;

    spice_return_if_fail(msg->surface_id == 0);

    flush_all_qxl_commands(worker);
    display_channel_destroy_surface_wait(worker->display_channel, msg->surface_id);
}

static void handle_dev_destroy_surfaces(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    flush_all_qxl_commands(worker);
    display_channel_destroy_surfaces(worker->display_channel);
    cursor_channel_reset(worker->cursor_channel);
}

static void display_update_monitors_config(DisplayChannel *display,
                                           QXLMonitorsConfig *config,
                                           uint16_t count, uint16_t max_allowed)
{

    if (display->monitors_config)
        monitors_config_unref(display->monitors_config);

    display->monitors_config =
        monitors_config_new(config->heads, count, max_allowed);
}

static void red_worker_push_monitors_config(RedWorker *worker)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    FOREACH_DCC(worker->display_channel, item, next, dcc) {
        dcc_push_monitors_config(dcc);
    }
}

static void set_monitors_config_to_primary(DisplayChannel *display)
{
    DrawContext *context = &display->surfaces[0].context;
    QXLHead head = { 0, };

    spice_return_if_fail(display->surfaces[0].context.canvas);

    if (display->monitors_config)
        monitors_config_unref(display->monitors_config);

    head.width = context->width;
    head.height = context->height;
    display->monitors_config = monitors_config_new(&head, 1, 1);
}

static void dev_create_primary_surface(RedWorker *worker, uint32_t surface_id,
                                       QXLDevSurfaceCreate surface)
{
    DisplayChannel *display = worker->display_channel;
    uint8_t *line_0;
    int error;

    spice_debug(NULL);
    spice_warn_if(surface_id != 0);
    spice_warn_if(surface.height == 0);
    spice_warn_if(((uint64_t)abs(surface.stride) * (uint64_t)surface.height) !=
             abs(surface.stride) * surface.height);

    line_0 = (uint8_t*)get_virt(&worker->mem_slots, surface.mem,
                                surface.height * abs(surface.stride),
                                surface.group_id, &error);
    if (error) {
        return;
    }
    if (worker->record_fd) {
        red_record_dev_input_primary_surface_create(worker->record_fd,
                    &surface, line_0);
    }

    if (surface.stride < 0) {
        line_0 -= (int32_t)(surface.stride * (surface.height -1));
    }

    display_channel_create_surface(display, 0, surface.width, surface.height, surface.stride, surface.format,
                                   line_0, surface.flags & QXL_SURF_FLAG_KEEP_DATA, TRUE);
    set_monitors_config_to_primary(display);

    if (display_is_connected(worker) && !worker->display_channel->common.during_target_migrate) {
        /* guest created primary, so it will (hopefully) send a monitors_config
         * now, don't send our own temporary one */
        if (!worker->driver_cap_monitors_config) {
            red_worker_push_monitors_config(worker);
        }
        red_pipes_add_verb(&worker->display_channel->common.base,
                           SPICE_MSG_DISPLAY_MARK);
        red_channel_push(&worker->display_channel->common.base);
    }

    cursor_channel_init(worker->cursor_channel, NULL);
}

static void handle_dev_create_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurface *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

static void destroy_primary_surface(RedWorker *worker, uint32_t surface_id)
{
    DisplayChannel *display = worker->display_channel;

    if (!validate_surface(display, surface_id))
        return;
    spice_warn_if(surface_id != 0);

    spice_debug(NULL);
    if (!display->surfaces[surface_id].context.canvas) {
        spice_warning("double destroy of primary surface");
        return;
    }

    flush_all_qxl_commands(worker);
    display_channel_destroy_surface_wait(display, 0);
    display_channel_surface_unref(display, 0);

    spice_warn_if_fail(ring_is_empty(&display->streams));
    spice_warn_if_fail(!display->surfaces[surface_id].context.canvas);

    cursor_channel_reset(worker->cursor_channel);
}

static void handle_dev_destroy_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurface *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    destroy_primary_surface(worker, surface_id);
}

static void handle_dev_destroy_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    destroy_primary_surface(worker, surface_id);
}

static void handle_dev_flush_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    flush_all_qxl_commands(worker);
    display_channel_flush_all_surfaces(worker->display_channel);
}

static void handle_dev_stop(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    spice_info("stop");
    spice_assert(worker->running);

    worker->running = FALSE;

    display_channel_free_glz_drawables(worker->display_channel);
    display_channel_flush_all_surfaces(worker->display_channel);

    /* todo: when the waiting is expected to take long (slow connection and
     * overloaded pipe), don't wait, and in case of migration,
     * purge the pipe, send destroy_all_surfaces
     * to the client (there is no such message right now), and start
     * from scratch on the destination side */
    if (!red_channel_wait_all_sent(RED_CHANNEL(worker->display_channel),
                                   DISPLAY_CLIENT_TIMEOUT)) {
        red_channel_apply_clients(RED_CHANNEL(worker->display_channel),
                                 red_channel_client_disconnect_if_pending_send);
    }
    if (!red_channel_wait_all_sent(RED_CHANNEL(worker->cursor_channel),
                                   DISPLAY_CLIENT_TIMEOUT)) {
        red_channel_apply_clients(RED_CHANNEL(worker->cursor_channel),
                                 red_channel_client_disconnect_if_pending_send);
    }
}

static void handle_dev_start(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    spice_assert(!worker->running);
    if (worker->cursor_channel) {
        COMMON_CHANNEL(worker->cursor_channel)->during_target_migrate = FALSE;
    }
    if (worker->display_channel) {
        worker->display_channel->common.during_target_migrate = FALSE;
        if (red_channel_waits_for_migrate_data(&worker->display_channel->common.base)) {
            display_channel_wait_for_migrate_data(worker->display_channel);
        }
    }
    worker->running = TRUE;
    guest_set_client_capabilities(worker);
}

static void handle_dev_wakeup(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    stat_inc_counter(worker->wakeup_counter, 1);
    red_dispatcher_clear_pending(worker->red_dispatcher, RED_DISPATCHER_PENDING_WAKEUP);
}

static void handle_dev_oom(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    DisplayChannel *display = worker->display_channel;

    RedChannel *display_red_channel = &display->common.base;
    int ring_is_empty;

    spice_return_if_fail(worker->running);
    // streams? but without streams also leak
    spice_debug("OOM1 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                display->drawable_count,
                display->red_drawable_count,
                display->glz_drawable_count,
                display->current_size,
                red_channel_sum_pipes_size(display_red_channel));
    while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
        red_channel_push(display_red_channel);
    }
    if (worker->qxl->st->qif->flush_resources(worker->qxl) == 0) {
        display_channel_free_some(worker->display_channel);
        worker->qxl->st->qif->flush_resources(worker->qxl);
    }
    spice_debug("OOM2 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                display->drawable_count,
                display->red_drawable_count,
                display->glz_drawable_count,
                display->current_size,
                red_channel_sum_pipes_size(display_red_channel));
    red_dispatcher_clear_pending(worker->red_dispatcher, RED_DISPATCHER_PENDING_OOM);
}

static void handle_dev_reset_cursor(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    cursor_channel_reset(worker->cursor_channel);
}

static void handle_dev_reset_image_cache(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    DisplayChannel *display = worker->display_channel;

    image_cache_reset(&display->image_cache);
}

static void handle_dev_destroy_surface_wait_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWaitAsync *msg = payload;
    RedWorker *worker = opaque;

    display_channel_destroy_surface_wait(worker->display_channel, msg->surface_id);
}

static void handle_dev_destroy_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    flush_all_qxl_commands(worker);
    display_channel_destroy_surfaces(worker->display_channel);
    cursor_channel_reset(worker->cursor_channel);
}

static void handle_dev_create_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

static void handle_dev_display_connect(void *opaque, void *payload)
{
    RedWorkerMessageDisplayConnect *msg = payload;
    RedWorker *worker = opaque;
    RedsStream *stream = msg->stream;
    RedClient *client = msg->client;
    int migration = msg->migration;

    spice_info("connect");
    handle_new_display_channel(worker, client, stream, migration,
                               msg->common_caps, msg->num_common_caps,
                               msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

static void handle_dev_display_disconnect(void *opaque, void *payload)
{
    RedWorkerMessageDisplayDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;
    RedWorker *worker = opaque;

    spice_info("disconnect display client");
    spice_assert(rcc);

    guest_set_client_capabilities(worker);

    red_channel_client_disconnect(rcc);
}

static void handle_dev_display_migrate(void *opaque, void *payload)
{
    RedWorkerMessageDisplayMigrate *msg = payload;
    RedWorker *worker = opaque;

    RedChannelClient *rcc = msg->rcc;
    spice_info("migrate display client");
    spice_assert(rcc);
    red_migrate_display(worker->display_channel, rcc);
}

static inline uint32_t qxl_monitors_config_size(uint32_t heads)
{
    return sizeof(QXLMonitorsConfig) + sizeof(QXLHead) * heads;
}

static void handle_dev_monitors_config_async(void *opaque, void *payload)
{
    RedWorkerMessageMonitorsConfigAsync *msg = payload;
    RedWorker *worker = opaque;
    int error;
    uint16_t count, max_allowed;
    QXLMonitorsConfig *dev_monitors_config =
        (QXLMonitorsConfig*)get_virt(&worker->mem_slots, msg->monitors_config,
                                     qxl_monitors_config_size(1),
                                     msg->group_id, &error);

    if (error) {
        /* TODO: raise guest bug (requires added QXL interface) */
        return;
    }
    worker->driver_cap_monitors_config = 1;
    count = dev_monitors_config->count;
    max_allowed = dev_monitors_config->max_allowed;
    if (count == 0) {
        spice_warning("ignoring an empty monitors config message from driver");
        return;
    }
    if (count > max_allowed) {
        spice_warning("ignoring malformed monitors_config from driver, "
                      "count > max_allowed %d > %d",
                      count,
                      max_allowed);
        return;
    }
    /* get pointer again to check virtual size */
    dev_monitors_config =
        (QXLMonitorsConfig*)get_virt(&worker->mem_slots, msg->monitors_config,
                                     qxl_monitors_config_size(count),
                                     msg->group_id, &error);
    if (error) {
        /* TODO: raise guest bug (requires added QXL interface) */
        return;
    }
    display_update_monitors_config(worker->display_channel, dev_monitors_config,
                                   MIN(count, msg->max_monitors),
                                   MIN(max_allowed, msg->max_monitors));
    red_worker_push_monitors_config(worker);
}

/* TODO: special, perhaps use another dispatcher? */
static void handle_dev_cursor_connect(void *opaque, void *payload)
{
    RedWorkerMessageCursorConnect *msg = payload;
    RedWorker *worker = opaque;

    spice_info("cursor connect");
    cursor_connect(worker,
                   msg->client, msg->stream, msg->migration,
                   msg->common_caps, msg->num_common_caps,
                   msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

static void handle_dev_cursor_disconnect(void *opaque, void *payload)
{
    RedWorkerMessageCursorDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;

    spice_info("disconnect cursor client");
    spice_return_if_fail(rcc);
    red_channel_client_disconnect(rcc);
}

static void handle_dev_cursor_migrate(void *opaque, void *payload)
{
    RedWorkerMessageCursorMigrate *msg = payload;
    RedChannelClient *rcc = msg->rcc;

    spice_info("migrate cursor client");
    cursor_channel_client_migrate(CURSOR_CHANNEL_CLIENT(rcc));
}

static void handle_dev_set_compression(void *opaque, void *payload)
{
    RedWorkerMessageSetCompression *msg = payload;
    RedWorker *worker = opaque;

    worker->image_compression = msg->image_compression;
    switch (worker->image_compression) {
    case SPICE_IMAGE_COMPRESSION_AUTO_LZ:
        spice_info("ic auto_lz");
        break;
    case SPICE_IMAGE_COMPRESSION_AUTO_GLZ:
        spice_info("ic auto_glz");
        break;
    case SPICE_IMAGE_COMPRESSION_QUIC:
        spice_info("ic quic");
        break;
#ifdef USE_LZ4
    case SPICE_IMAGE_COMPRESSION_LZ4:
        spice_info("ic lz4");
        break;
#endif
    case SPICE_IMAGE_COMPRESSION_LZ:
        spice_info("ic lz");
        break;
    case SPICE_IMAGE_COMPRESSION_GLZ:
        spice_info("ic glz");
        break;
    case SPICE_IMAGE_COMPRESSION_OFF:
        spice_info("ic off");
        break;
    default:
        spice_warning("ic invalid");
    }

    display_channel_compress_stats_print(worker->display_channel);
    display_channel_compress_stats_reset(worker->display_channel);
}

static void handle_dev_set_streaming_video(void *opaque, void *payload)
{
    RedWorkerMessageSetStreamingVideo *msg = payload;
    RedWorker *worker = opaque;

    display_channel_set_stream_video(worker->display_channel, msg->streaming_video);
}

static void handle_dev_set_mouse_mode(void *opaque, void *payload)
{
    RedWorkerMessageSetMouseMode *msg = payload;
    RedWorker *worker = opaque;

    spice_info("mouse mode %u", msg->mode);
    cursor_channel_set_mouse_mode(worker->cursor_channel, msg->mode);
}

static void dev_add_memslot(RedWorker *worker, QXLDevMemSlot mem_slot)
{
    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

static void handle_dev_add_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAddMemslot *msg = payload;
    QXLDevMemSlot mem_slot = msg->mem_slot;

    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

static void handle_dev_add_memslot_async(void *opaque, void *payload)
{
    RedWorkerMessageAddMemslotAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_add_memslot(worker, msg->mem_slot);
}

static void handle_dev_reset_memslots(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    red_memslot_info_reset(&worker->mem_slots);
}

static void handle_dev_driver_unload(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    worker->driver_cap_monitors_config = 0;
}

static int loadvm_command(RedWorker *worker, QXLCommandExt *ext)
{
    RedCursorCmd *cursor_cmd;
    RedSurfaceCmd *surface_cmd;

    switch (ext->cmd.type) {
    case QXL_CMD_CURSOR:
        cursor_cmd = spice_new0(RedCursorCmd, 1);
        if (red_get_cursor_cmd(&worker->mem_slots, ext->group_id, cursor_cmd, ext->cmd.data)) {
            free(cursor_cmd);
            return FALSE;
        }
        cursor_channel_process_cmd(worker->cursor_channel, cursor_cmd, ext->group_id);
        break;
    case QXL_CMD_SURFACE:
        surface_cmd = spice_new0(RedSurfaceCmd, 1);
        if (red_get_surface_cmd(&worker->mem_slots, ext->group_id, surface_cmd, ext->cmd.data)) {
            free(surface_cmd);
            return FALSE;
        }
        red_process_surface(worker, surface_cmd, ext->group_id, TRUE);
        break;
    default:
        spice_warning("unhandled loadvm command type (%d)", ext->cmd.type);
    }

    return TRUE;
}

static void handle_dev_loadvm_commands(void *opaque, void *payload)
{
    RedWorkerMessageLoadvmCommands *msg = payload;
    RedWorker *worker = opaque;
    uint32_t i;
    uint32_t count = msg->count;
    QXLCommandExt *ext = msg->ext;

    spice_info("loadvm_commands");
    for (i = 0 ; i < count ; ++i) {
        if (!loadvm_command(worker, &ext[i])) {
            /* XXX allow failure in loadvm? */
            spice_warning("failed loadvm command type (%d)", ext[i].cmd.type);
        }
    }
}

static void worker_handle_dispatcher_async_done(void *opaque,
                                                uint32_t message_type,
                                                void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAsync *msg_async = payload;

    spice_debug(NULL);
    red_dispatcher_async_complete(worker->red_dispatcher, msg_async->cmd);
}

static void worker_dispatcher_record(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    red_record_event(worker->record_fd, 1, message_type, stat_now(worker->clockid));
}

static void register_callbacks(Dispatcher *dispatcher)
{
    dispatcher_register_async_done_callback(
                                    dispatcher,
                                    worker_handle_dispatcher_async_done);

    /* TODO: register cursor & display specific msg in respective channel files */
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_CONNECT,
                                handle_dev_display_connect,
                                sizeof(RedWorkerMessageDisplayConnect),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_DISCONNECT,
                                handle_dev_display_disconnect,
                                sizeof(RedWorkerMessageDisplayDisconnect),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_MIGRATE,
                                handle_dev_display_migrate,
                                sizeof(RedWorkerMessageDisplayMigrate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_CONNECT,
                                handle_dev_cursor_connect,
                                sizeof(RedWorkerMessageCursorConnect),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_DISCONNECT,
                                handle_dev_cursor_disconnect,
                                sizeof(RedWorkerMessageCursorDisconnect),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_MIGRATE,
                                handle_dev_cursor_migrate,
                                sizeof(RedWorkerMessageCursorMigrate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE,
                                handle_dev_update,
                                sizeof(RedWorkerMessageUpdate),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE_ASYNC,
                                handle_dev_update_async,
                                sizeof(RedWorkerMessageUpdateAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT,
                                handle_dev_add_memslot,
                                sizeof(RedWorkerMessageAddMemslot),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC,
                                handle_dev_add_memslot_async,
                                sizeof(RedWorkerMessageAddMemslotAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DEL_MEMSLOT,
                                handle_dev_del_memslot,
                                sizeof(RedWorkerMessageDelMemslot),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES,
                                handle_dev_destroy_surfaces,
                                sizeof(RedWorkerMessageDestroySurfaces),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC,
                                handle_dev_destroy_surfaces_async,
                                sizeof(RedWorkerMessageDestroySurfacesAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
                                handle_dev_destroy_primary_surface,
                                sizeof(RedWorkerMessageDestroyPrimarySurface),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC,
                                handle_dev_destroy_primary_surface_async,
                                sizeof(RedWorkerMessageDestroyPrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC,
                                handle_dev_create_primary_surface_async,
                                sizeof(RedWorkerMessageCreatePrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
                                handle_dev_create_primary_surface,
                                sizeof(RedWorkerMessageCreatePrimarySurface),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
                                handle_dev_reset_image_cache,
                                sizeof(RedWorkerMessageResetImageCache),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_CURSOR,
                                handle_dev_reset_cursor,
                                sizeof(RedWorkerMessageResetCursor),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_WAKEUP,
                                handle_dev_wakeup,
                                sizeof(RedWorkerMessageWakeup),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_OOM,
                                handle_dev_oom,
                                sizeof(RedWorkerMessageOom),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_START,
                                handle_dev_start,
                                sizeof(RedWorkerMessageStart),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC,
                                handle_dev_flush_surfaces_async,
                                sizeof(RedWorkerMessageFlushSurfacesAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_STOP,
                                handle_dev_stop,
                                sizeof(RedWorkerMessageStop),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_LOADVM_COMMANDS,
                                handle_dev_loadvm_commands,
                                sizeof(RedWorkerMessageLoadvmCommands),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_COMPRESSION,
                                handle_dev_set_compression,
                                sizeof(RedWorkerMessageSetCompression),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
                                handle_dev_set_streaming_video,
                                sizeof(RedWorkerMessageSetStreamingVideo),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_MOUSE_MODE,
                                handle_dev_set_mouse_mode,
                                sizeof(RedWorkerMessageSetMouseMode),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
                                handle_dev_destroy_surface_wait,
                                sizeof(RedWorkerMessageDestroySurfaceWait),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC,
                                handle_dev_destroy_surface_wait_async,
                                sizeof(RedWorkerMessageDestroySurfaceWaitAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_MEMSLOTS,
                                handle_dev_reset_memslots,
                                sizeof(RedWorkerMessageResetMemslots),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC,
                                handle_dev_monitors_config_async,
                                sizeof(RedWorkerMessageMonitorsConfigAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DRIVER_UNLOAD,
                                handle_dev_driver_unload,
                                sizeof(RedWorkerMessageDriverUnload),
                                DISPATCHER_NONE);
}



static void handle_dev_input(int fd, int event, void *opaque)
{
    RedWorker *worker = opaque;

    dispatcher_handle_recv_read(red_dispatcher_get_dispatcher(worker->red_dispatcher));
}

RedWorker* red_worker_new(QXLInstance *qxl, RedDispatcher *red_dispatcher)
{
    QXLDevInitInfo init_info;
    RedWorker *worker;
    Dispatcher *dispatcher;
    int i;
    const char *record_filename;

    qxl->st->qif->get_init_info(qxl, &init_info);

    worker = spice_new0(RedWorker, 1);

    record_filename = getenv("SPICE_WORKER_RECORD_FILENAME");
    if (record_filename) {
        static const char header[] = "SPICE_REPLAY 1\n";

        worker->record_fd = fopen(record_filename, "w+");
        if (worker->record_fd == NULL) {
            spice_error("failed to open recording file %s\n", record_filename);
        }
        if (fwrite(header, sizeof(header)-1, 1, worker->record_fd) != 1) {
            spice_error("failed to write replay header");
        }
    }
    dispatcher = red_dispatcher_get_dispatcher(red_dispatcher);
    dispatcher_set_opaque(dispatcher, worker);

    worker->red_dispatcher = red_dispatcher;
    worker->qxl = qxl;
    register_callbacks(dispatcher);
    if (worker->record_fd) {
        dispatcher_register_universal_handler(dispatcher, worker_dispatcher_record);
    }
    worker->image_compression = image_compression;
    worker->jpeg_state = jpeg_state;
    worker->zlib_glz_state = zlib_glz_state;
    worker->driver_cap_monitors_config = 0;
#ifdef RED_STATISTICS
    char worker_str[20];
    sprintf(worker_str, "display[%d]", worker->qxl->id);
    worker->stat = stat_add_node(INVALID_STAT_REF, worker_str, TRUE);
    worker->wakeup_counter = stat_add_counter(worker->stat, "wakeups", TRUE);
    worker->command_counter = stat_add_counter(worker->stat, "commands", TRUE);
#endif
    for (i = 0; i < MAX_EVENT_SOURCES; i++) {
        worker->poll_fds[i].fd = -1;
    }

    worker->poll_fds[0].fd = dispatcher_get_recv_fd(dispatcher);
    worker->poll_fds[0].events = POLLIN;
    worker->watches[0].worker = worker;
    worker->watches[0].watch_func = handle_dev_input;
    worker->watches[0].watch_func_opaque = worker;

    red_memslot_info_init(&worker->mem_slots,
                          init_info.num_memslots_groups,
                          init_info.num_memslots,
                          init_info.memslot_gen_bits,
                          init_info.memslot_id_bits,
                          init_info.internal_groupslot_id);

    spice_warn_if(init_info.n_surfaces > NUM_SURFACES);

    worker->event_timeout = INF_EVENT_WAIT;

    worker->cursor_channel = cursor_channel_new(worker);
    // TODO: handle seemless migration. Temp, setting migrate to FALSE
    worker->display_channel = display_channel_new(worker, FALSE, streaming_video,
                                                  init_info.n_surfaces);

    return worker;
}

SPICE_GNUC_NORETURN static void *red_worker_main(void *arg)
{
    RedWorker *worker = arg;

    spice_info("begin");
    spice_assert(MAX_PIPE_SIZE > WIDE_CLIENT_ACK_WINDOW &&
           MAX_PIPE_SIZE > NARROW_CLIENT_ACK_WINDOW); //ensure wakeup by ack message

    if (!spice_timer_queue_create()) {
        spice_error("failed to create timer queue");
    }

    if (pthread_getcpuclockid(pthread_self(), &worker->clockid)) {
        spice_warning("getcpuclockid failed");
    }

    RED_CHANNEL(worker->cursor_channel)->thread_id = pthread_self();
    RED_CHANNEL(worker->display_channel)->thread_id = pthread_self();

    for (;;) {
        int i, num_events;
        unsigned int timeout;

        timeout = spice_timer_queue_get_timeout_ms();
        worker->event_timeout = MIN(timeout, worker->event_timeout);
        timeout = display_channel_get_streams_timeout(worker->display_channel);
        worker->event_timeout = MIN(timeout, worker->event_timeout);
        num_events = poll(worker->poll_fds, MAX_EVENT_SOURCES, worker->event_timeout);
        stream_timeout(worker->display_channel);
        spice_timer_queue_cb();

        if (worker->display_channel) {
            /* during migration, in the dest, the display channel can be initialized
               while the global lz data not since migrate data msg hasn't been
               received yet */
            display_channel_free_glz_drawables_to_free(worker->display_channel);
        }

        worker->event_timeout = INF_EVENT_WAIT;
        if (num_events == -1) {
            if (errno != EINTR) {
                spice_error("poll failed, %s", strerror(errno));
            }
        }

        for (i = 0; i < MAX_EVENT_SOURCES; i++) {
            /* The watch may have been removed by the watch-func from
               another fd (ie a disconnect through the dispatcher),
               in this case watch_func is NULL. */
            if (worker->poll_fds[i].revents && worker->watches[i].watch_func) {
                int events = 0;
                if (worker->poll_fds[i].revents & POLLIN) {
                    events |= SPICE_WATCH_EVENT_READ;
                }
                if (worker->poll_fds[i].revents & POLLOUT) {
                    events |= SPICE_WATCH_EVENT_WRITE;
                }
                worker->watches[i].watch_func(worker->poll_fds[i].fd, events,
                                        worker->watches[i].watch_func_opaque);
            }
        }

        /* Clear the poll_fd for any removed watches, see the comment in
           watch_remove for why we don't do this there. */
        for (i = 0; i < MAX_EVENT_SOURCES; i++) {
            if (!worker->watches[i].watch_func) {
                worker->poll_fds[i].fd = -1;
            }
        }

        if (worker->running) {
            int ring_is_empty;
            red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty);
            red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty);
        }
        red_push(worker);
    }

    spice_warn_if_reached();
}

bool red_worker_run(RedWorker *worker)
{
    sigset_t thread_sig_mask;
    sigset_t curr_sig_mask;
    int r;

    spice_return_val_if_fail(worker, FALSE);
    spice_return_val_if_fail(!worker->thread, FALSE);

    sigfillset(&thread_sig_mask);
    sigdelset(&thread_sig_mask, SIGILL);
    sigdelset(&thread_sig_mask, SIGFPE);
    sigdelset(&thread_sig_mask, SIGSEGV);
    pthread_sigmask(SIG_SETMASK, &thread_sig_mask, &curr_sig_mask);
    if ((r = pthread_create(&worker->thread, NULL, red_worker_main, worker))) {
        spice_error("create thread failed %d", r);
    }
    pthread_sigmask(SIG_SETMASK, &curr_sig_mask, NULL);

    return r == 0;
}

RedChannel* red_worker_get_cursor_channel(RedWorker *worker)
{
    spice_return_val_if_fail(worker, NULL);

    return RED_CHANNEL(worker->cursor_channel);
}

RedChannel* red_worker_get_display_channel(RedWorker *worker)
{
    spice_return_val_if_fail(worker, NULL);

    return RED_CHANNEL(worker->display_channel);
}

clockid_t red_worker_get_clockid(RedWorker *worker)
{
    spice_return_val_if_fail(worker, 0);

    return worker->clockid;
}
