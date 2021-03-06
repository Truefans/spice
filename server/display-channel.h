/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

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
#ifndef DISPLAY_CHANNEL_H_
# define DISPLAY_CHANNEL_H_

#include <setjmp.h>

#include "common/rect.h"
#include "red_worker.h"
#include "reds_stream.h"
#include "cache-item.h"
#include "pixmap-cache.h"
#include "reds_sw_canvas.h"
#include "stat.h"
#include "reds.h"
#include "mjpeg_encoder.h"
#include "red_memslots.h"
#include "red_parse_qxl.h"
#include "red_record_qxl.h"
#include "demarshallers.h"
#include "red_channel.h"
#include "red_dispatcher.h"
#include "dispatcher.h"
#include "main_channel.h"
#include "migration_protocol.h"
#include "main_dispatcher.h"
#include "spice_bitmap_utils.h"
#include "spice_image_cache.h"
#include "utils.h"
#include "tree.h"
#include "stream.h"
#include "dcc.h"
#include "display-limits.h"


typedef struct DependItem {
    Drawable *drawable;
    RingItem ring_item;
} DependItem;

struct Drawable {
    uint8_t refs;
    RingItem surface_list_link;
    RingItem list_link;
    DrawItem tree_item;
    Ring pipes;
    PipeItem *pipe_item_rest;
    uint32_t size_pipe_item_rest;
    RedDrawable *red_drawable;

    Ring glz_ring;

    red_time_t creation_time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    Stream *stream;
    Stream *sized_stream;
    int streamable;
    BitmapGradualType copy_bitmap_graduality;
    uint32_t group_id;
    DependItem depend_items[3];

    int surface_id;
    int surface_deps[3];

    uint32_t process_commands_generation;
};

#define LINK_TO_DPI(ptr) SPICE_CONTAINEROF((ptr), DrawablePipeItem, base)
#define DRAWABLE_FOREACH_DPI_SAFE(drawable, link, next, dpi)            \
    SAFE_FOREACH(link, next, drawable,  &(drawable)->pipes, dpi, LINK_TO_DPI(link))

#define LINK_TO_GLZ(ptr) SPICE_CONTAINEROF((ptr), RedGlzDrawable, \
                                           drawable_link)
#define DRAWABLE_FOREACH_GLZ_SAFE(drawable, link, next, glz) \
    SAFE_FOREACH(link, next, drawable, &(drawable)->glz_ring, glz, LINK_TO_GLZ(link))

enum {
    PIPE_ITEM_TYPE_DRAW = PIPE_ITEM_TYPE_COMMON_LAST,
    PIPE_ITEM_TYPE_IMAGE,
    PIPE_ITEM_TYPE_STREAM_CREATE,
    PIPE_ITEM_TYPE_STREAM_CLIP,
    PIPE_ITEM_TYPE_STREAM_DESTROY,
    PIPE_ITEM_TYPE_UPGRADE,
    PIPE_ITEM_TYPE_MIGRATE_DATA,
    PIPE_ITEM_TYPE_PIXMAP_SYNC,
    PIPE_ITEM_TYPE_PIXMAP_RESET,
    PIPE_ITEM_TYPE_INVAL_PALETTE_CACHE,
    PIPE_ITEM_TYPE_CREATE_SURFACE,
    PIPE_ITEM_TYPE_DESTROY_SURFACE,
    PIPE_ITEM_TYPE_MONITORS_CONFIG,
    PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT,
};

typedef struct MonitorsConfig {
    int refs;
    int count;
    int max_allowed;
    QXLHead heads[0];
} MonitorsConfig;

typedef struct MonitorsConfigItem {
    PipeItem pipe_item;
    MonitorsConfig *monitors_config;
} MonitorsConfigItem;

MonitorsConfig*            monitors_config_new                       (QXLHead *heads, ssize_t nheads,
                                                                      ssize_t max);
MonitorsConfig *           monitors_config_ref                       (MonitorsConfig *config);
void                       monitors_config_unref                     (MonitorsConfig *config);

#define TRACE_ITEMS_SHIFT 3
#define NUM_TRACE_ITEMS (1 << TRACE_ITEMS_SHIFT)
#define ITEMS_TRACE_MASK (NUM_TRACE_ITEMS - 1)

typedef struct DrawContext {
    SpiceCanvas *canvas;
    int canvas_draws_on_surface;
    int top_down;
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint32_t format;
    void *line_0;
} DrawContext;

typedef struct RedSurface {
    uint32_t refs;
    Ring current;
    Ring current_list;
    DrawContext context;

    Ring depend_on_me;
    QRegion draw_dirty_region;

    //fix me - better handling here
    QXLReleaseInfoExt create, destroy;
} RedSurface;

#define NUM_DRAWABLES 1000
typedef struct _Drawable _Drawable;
struct _Drawable {
    union {
        Drawable drawable;
        _Drawable *next;
    } u;
};

struct DisplayChannel {
    CommonChannel common; // Must be the first thing
    uint32_t bits_unique;

    MonitorsConfig *monitors_config;

    uint32_t num_renderers;
    uint32_t renderers[RED_RENDERER_LAST];
    uint32_t renderer;
    int enable_jpeg;
    int enable_zlib_glz_wrap;

    Ring current_list; // of TreeItem
    uint32_t current_size;

    uint32_t drawable_count;
    _Drawable drawables[NUM_DRAWABLES];
    _Drawable *free_drawables;

    uint32_t red_drawable_count;
    uint32_t glz_drawable_count;

    int stream_video;
    uint32_t stream_count;
    Stream streams_buf[NUM_STREAMS];
    Stream *free_streams;
    Ring streams;
    ItemTrace items_trace[NUM_TRACE_ITEMS];
    uint32_t next_item_trace;
    uint64_t streams_size_total;

    RedSurface surfaces[NUM_SURFACES];
    uint32_t n_surfaces;
    SpiceImageSurfaces image_surfaces;

    ImageCache image_cache;
    RedCompressBuf *free_compress_bufs;

/* TODO: some day unify this, make it more runtime.. */
#ifdef RED_WORKER_STAT
    stat_info_t add_stat;
    stat_info_t exclude_stat;
    stat_info_t __exclude_stat;
    uint32_t add_count;
    uint32_t add_with_shadow_count;
#endif
#ifdef RED_STATISTICS
    uint64_t *cache_hits_counter;
    uint64_t *add_to_cache_counter;
    uint64_t *non_cache_counter;
#endif
#ifdef COMPRESS_STAT
    stat_info_t lz_stat;
    stat_info_t glz_stat;
    stat_info_t quic_stat;
    stat_info_t jpeg_stat;
    stat_info_t zlib_glz_stat;
    stat_info_t jpeg_alpha_stat;
    stat_info_t lz4_stat;
#endif
};

#define LINK_TO_DCC(ptr) SPICE_CONTAINEROF(ptr, DisplayChannelClient,   \
                                           common.base.channel_link)
#define DCC_FOREACH_SAFE(link, next, dcc, channel)                      \
    SAFE_FOREACH(link, next, channel,  &(channel)->clients, dcc, LINK_TO_DCC(link))


#define FOREACH_DCC(display_channel, link, next, dcc)                   \
    DCC_FOREACH_SAFE(link, next, dcc, RED_CHANNEL(display_channel))

static inline int get_stream_id(DisplayChannel *display, Stream *stream)
{
    return (int)(stream - display->streams_buf);
}

typedef struct SurfaceDestroyItem {
    SpiceMsgSurfaceDestroy surface_destroy;
    PipeItem pipe_item;
} SurfaceDestroyItem;

typedef struct UpgradeItem {
    PipeItem base;
    int refs;
    Drawable *drawable;
    SpiceClipRects *rects;
} UpgradeItem;


DisplayChannel*            display_channel_new                       (RedWorker *worker,
                                                                      int migrate,
                                                                      int stream_video,
                                                                      uint32_t n_surfaces);
void                       display_channel_create_surface            (DisplayChannel *display, uint32_t surface_id,
                                                                      uint32_t width, uint32_t height,
                                                                      int32_t stride, uint32_t format, void *line_0,
                                                                      int data_is_valid, int send_client);
void                       display_channel_draw                      (DisplayChannel *display,
                                                                      const SpiceRect *area,
                                                                      int surface_id);
void                       display_channel_draw_until                (DisplayChannel *display,
                                                                      const SpiceRect *area,
                                                                      int surface_id,
                                                                      Drawable *last);
void                       display_channel_update                    (DisplayChannel *display,
                                                                      uint32_t surface_id,
                                                                      const QXLRect *area,
                                                                      uint32_t clear_dirty,
                                                                      QXLRect **qxl_dirty_rects,
                                                                      uint32_t *num_dirty_rects);
void                       display_channel_free_some                 (DisplayChannel *display);
void                       display_channel_set_stream_video          (DisplayChannel *display,
                                                                      int stream_video);
int                        display_channel_get_streams_timeout       (DisplayChannel *display);
void                       display_channel_compress_stats_print      (const DisplayChannel *display);
void                       display_channel_compress_stats_reset      (DisplayChannel *display);
Drawable *                 display_channel_drawable_try_new          (DisplayChannel *display,
                                                                      int group_id,
                                                                      int process_commands_generation);
void                       display_channel_drawable_unref            (DisplayChannel *display, Drawable *drawable);
void                       display_channel_surface_unref             (DisplayChannel *display,
                                                                      uint32_t surface_id);
bool                       display_channel_surface_has_canvas        (DisplayChannel *display,
                                                                      uint32_t surface_id);
void                       display_channel_add_drawable              (DisplayChannel *display,
                                                                      Drawable *drawable);
void                       display_channel_current_flush             (DisplayChannel *display,
                                                                      int surface_id);
int                        display_channel_wait_for_migrate_data     (DisplayChannel *display);
void                       display_channel_flush_all_surfaces        (DisplayChannel *display);
void                       display_channel_free_glz_drawables_to_free(DisplayChannel *display);
void                       display_channel_free_glz_drawables        (DisplayChannel *display);
void                       display_channel_destroy_surface_wait      (DisplayChannel *display,
                                                                      uint32_t surface_id);
void                       display_channel_destroy_surfaces          (DisplayChannel *display);
void                       display_channel_destroy_surface           (DisplayChannel *display,
                                                                      uint32_t surface_id);
uint32_t                   display_channel_generate_uid              (DisplayChannel *display);

static inline int validate_surface(DisplayChannel *display, uint32_t surface_id)
{
    if SPICE_UNLIKELY(surface_id >= display->n_surfaces) {
        spice_warning("invalid surface_id %u", surface_id);
        return 0;
    }
    if (!display->surfaces[surface_id].context.canvas) {
        spice_warning("canvas address is %p for %d (and is NULL)\n",
                   &(display->surfaces[surface_id].context.canvas), surface_id);
        spice_warning("failed on %d", surface_id);
        return 0;
    }
    return 1;
}

static inline int is_equal_path(SpicePath *path1, SpicePath *path2)
{
    SpicePathSeg *seg1, *seg2;
    int i, j;

    if (path1->num_segments != path2->num_segments)
        return FALSE;

    for (i = 0; i < path1->num_segments; i++) {
        seg1 = path1->segments[i];
        seg2 = path2->segments[i];

        if (seg1->flags != seg2->flags ||
            seg1->count != seg2->count) {
            return FALSE;
        }
        for (j = 0; j < seg1->count; j++) {
            if (seg1->points[j].x != seg2->points[j].x ||
                seg1->points[j].y != seg2->points[j].y) {
                return FALSE;
            }
        }
    }

    return TRUE;
}

// partial imp
static inline int is_equal_brush(SpiceBrush *b1, SpiceBrush *b2)
{
    return b1->type == b2->type &&
           b1->type == SPICE_BRUSH_TYPE_SOLID &&
           b1->u.color == b2->u.color;
}

// partial imp
static inline int is_equal_line_attr(SpiceLineAttr *a1, SpiceLineAttr *a2)
{
    return a1->flags == a2->flags &&
           a1->style_nseg == a2->style_nseg &&
           a1->style_nseg == 0;
}

// partial imp
static inline int is_same_geometry(Drawable *d1, Drawable *d2)
{
    if (d1->red_drawable->type != d2->red_drawable->type) {
        return FALSE;
    }

    switch (d1->red_drawable->type) {
    case QXL_DRAW_STROKE:
        return is_equal_line_attr(&d1->red_drawable->u.stroke.attr,
                                  &d2->red_drawable->u.stroke.attr) &&
               is_equal_path(d1->red_drawable->u.stroke.path,
                             d2->red_drawable->u.stroke.path);
    case QXL_DRAW_FILL:
        return rect_is_equal(&d1->red_drawable->bbox, &d2->red_drawable->bbox);
    default:
        return FALSE;
    }
}

static inline int is_same_drawable(Drawable *d1, Drawable *d2)
{
    if (!is_same_geometry(d1, d2)) {
        return FALSE;
    }

    switch (d1->red_drawable->type) {
    case QXL_DRAW_STROKE:
        return is_equal_brush(&d1->red_drawable->u.stroke.brush,
                              &d2->red_drawable->u.stroke.brush);
    case QXL_DRAW_FILL:
        return is_equal_brush(&d1->red_drawable->u.fill.brush,
                              &d2->red_drawable->u.fill.brush);
    default:
        return FALSE;
    }
}

static inline int is_drawable_independent_from_surfaces(Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        if (drawable->surface_deps[x] != -1) {
            return FALSE;
        }
    }
    return TRUE;
}

static inline int has_shadow(RedDrawable *drawable)
{
    return drawable->type == QXL_COPY_BITS;
}

static inline int is_primary_surface(DisplayChannel *display, uint32_t surface_id)
{
    if (surface_id == 0) {
        return TRUE;
    }
    return FALSE;
}

static inline void region_add_clip_rects(QRegion *rgn, SpiceClipRects *data)
{
    int i;

    for (i = 0; i < data->num_rects; i++) {
        region_add(rgn, data->rects + i);
    }
}

static inline void draw_depend_on_me(DisplayChannel *display, uint32_t surface_id)
{
    RedSurface *surface;
    RingItem *ring_item;

    surface = &display->surfaces[surface_id];

    while ((ring_item = ring_get_tail(&surface->depend_on_me))) {
        Drawable *drawable;
        DependItem *depended_item = SPICE_CONTAINEROF(ring_item, DependItem, ring_item);
        drawable = depended_item->drawable;
        display_channel_draw(display, &drawable->red_drawable->bbox, drawable->surface_id);
    }
}

#endif /* DISPLAY_CHANNEL_H_ */
